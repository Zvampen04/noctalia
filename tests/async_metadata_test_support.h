#pragma once

#include "app/deferred_call_poll_source.h"
#include "app/timer_poll_source.h"
#include "core/timer_manager.h"
#include "ipc/ipc_poll_source.h"
#include "ipc/ipc_service.h"
#include "tests/test_check.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace noctalia::test::metadata {

  using Clock = std::chrono::steady_clock;
  using namespace std::chrono_literals;

  class TemporaryDirectory {
  public:
    TemporaryDirectory() {
      char pattern[] = "/tmp/noctalia-async-metadata-XXXXXX";
      const char* created = ::mkdtemp(pattern);
      TEST_CHECK(created != nullptr);
      path = created;
    }
    ~TemporaryDirectory() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
  };

  class ScopedEnvironment {
  public:
    ScopedEnvironment(std::string name, const std::string& value) : m_name(std::move(name)) {
      if (const char* previous = ::getenv(m_name.c_str())) {
        m_previous = previous;
      }
      TEST_CHECK(::setenv(m_name.c_str(), value.c_str(), 1) == 0);
    }
    ~ScopedEnvironment() {
      if (m_previous) {
        (void)::setenv(m_name.c_str(), m_previous->c_str(), 1);
      } else {
        (void)::unsetenv(m_name.c_str());
      }
    }

  private:
    std::string m_name;
    std::optional<std::string> m_previous;
  };

  // A finite I/O gate. Tests release it explicitly; the timeout keeps a broken
  // implementation from leaving the native test indefinitely blocked.
  class IoGate {
  public:
    ~IoGate() { release(); }

    void enter() {
      std::unique_lock lock(m_mutex);
      m_started = true;
      m_worker = std::this_thread::get_id();
      m_condition.notify_all();
      TEST_CHECK(m_condition.wait_for(lock, 5s, [this] { return m_released; }));
    }
    [[nodiscard]] bool started() const {
      std::scoped_lock lock(m_mutex);
      return m_started;
    }
    [[nodiscard]] std::thread::id worker() const {
      std::scoped_lock lock(m_mutex);
      return m_worker;
    }
    void release() {
      std::scoped_lock lock(m_mutex);
      m_released = true;
      m_condition.notify_all();
    }

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_started = false;
    bool m_released = false;
    std::thread::id m_worker;
  };

  // Use the shell's actual timer, IPC and deferred-callback poll sources. This
  // exercises metadata delivery without constructing a Wayland or GL renderer.
  class ResponsiveLoop {
  public:
    ResponsiveLoop()
        : m_runtime("XDG_RUNTIME_DIR", m_directory.path.string()), m_display("WAYLAND_DISPLAY", "async-metadata-test"),
          m_ipcPoll(m_ipc) {
      m_ipc.bind(noctalia::cli::msg::status, [](const std::string&) { return "responsive\n"; });
      TEST_CHECK(m_ipc.start());
      m_heartbeat.startRepeating(5ms, [this] {
        const auto now = Clock::now();
        if (m_previousBeat != Clock::time_point{}) {
          m_maxHeartbeatGap = std::max(m_maxHeartbeatGap, now - m_previousBeat);
        }
        m_previousBeat = now;
        ++m_beats;
      });
    }

    void step() {
      std::vector<pollfd> fds;
      PollSource* sources[] = {&m_timerPoll, &m_ipcPoll, &m_deferredPoll};
      std::size_t offsets[3];
      int timeout = 10;
      for (std::size_t i = 0; i < 3; ++i) {
        offsets[i] = sources[i]->addPollFds(fds);
        const int requested = sources[i]->pollTimeoutMs();
        if (requested >= 0) {
          timeout = std::min(timeout, requested);
        }
      }
      const int result = ::poll(fds.data(), fds.size(), timeout);
      TEST_CHECK(result >= 0 || errno == EINTR);
      if (result < 0) {
        return;
      }
      for (std::size_t i = 0; i < 3; ++i) {
        sources[i]->dispatch(fds, offsets[i]);
      }
    }

    template <typename Predicate> void until(Predicate predicate, std::chrono::milliseconds limit = 2s) {
      const auto deadline = Clock::now() + limit;
      while (!predicate() && Clock::now() < deadline) {
        step();
      }
      TEST_CHECK(predicate());
    }

    void pump(std::chrono::milliseconds duration) {
      const auto deadline = Clock::now() + duration;
      while (Clock::now() < deadline) {
        step();
      }
    }

    // A real socket client must receive cached status while metadata I/O is
    // held. No mock dispatcher or warmed catalog can satisfy this assertion.
    void checkStatus() {
      const auto started = Clock::now();
      const auto socketPath = m_ipc.socketPath();
      auto client = std::async(std::launch::async, [socketPath] {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        TEST_CHECK(fd >= 0);
        const timeval timeout{.tv_sec = 1, .tv_usec = 0};
        TEST_CHECK(::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        TEST_CHECK(socketPath.size() < sizeof(address.sun_path));
        std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
        TEST_CHECK(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        constexpr std::string_view command = "status";
        TEST_CHECK(::write(fd, command.data(), command.size()) == static_cast<ssize_t>(command.size()));
        TEST_CHECK(::shutdown(fd, SHUT_WR) == 0);
        std::string response;
        char bytes[64];
        for (;;) {
          const auto count = ::read(fd, bytes, sizeof(bytes));
          TEST_CHECK(count >= 0);
          if (count == 0) {
            break;
          }
          response.append(bytes, static_cast<std::size_t>(count));
        }
        ::close(fd);
        return response;
      });
      until([&client] { return client.wait_for(0ms) == std::future_status::ready; }, 250ms);
      TEST_CHECK(client.get() == "responsive\n");
      TEST_CHECK(Clock::now() - started < 250ms);
    }

    [[nodiscard]] std::size_t beats() const { return m_beats; }
    [[nodiscard]] Clock::duration maxHeartbeatGap() const { return m_maxHeartbeatGap; }

  private:
    TemporaryDirectory m_directory;
    ScopedEnvironment m_runtime;
    ScopedEnvironment m_display;
    IpcService m_ipc;
    IpcPollSource m_ipcPoll;
    TimerPollSource m_timerPoll;
    DeferredCallPollSource m_deferredPoll;
    Timer m_heartbeat;
    std::size_t m_beats = 0;
    Clock::time_point m_previousBeat;
    Clock::duration m_maxHeartbeatGap{};
  };

} // namespace noctalia::test::metadata
