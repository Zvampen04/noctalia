#pragma once

#include "app/poll_source.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "system/icon_resolver.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <utility>

class IconThemePollSource final : public PollSource {
private:
  using Clock = std::chrono::steady_clock;

  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool requested = false;
    bool pending = false;
    bool undeliveredChanged = false;
    bool stopping = false;
  };

public:
  using CheckFunction = std::function<bool()>;

  explicit IconThemePollSource(CheckFunction check = {}, std::chrono::milliseconds interval = std::chrono::seconds{60})
      : m_interval(interval), m_nextCheck(Clock::now() + interval) {
    const std::weak_ptr<int> alive = m_alive;
    m_worker = std::thread([state = m_state, check = std::move(check), this, alive]() {
      for (;;) {
        {
          std::unique_lock lock(state->mutex);
          state->cv.wait(lock, [&]() { return state->stopping || state->requested; });
          if (state->stopping) {
            return;
          }
          state->requested = false;
        }
        bool changed = false;
        try {
          changed = check ? check() : IconResolver::checkThemeChanged();
        } catch (const std::exception& error) {
          try {
            Logger("icon-theme").warn("failed to check icon theme: {}", error.what());
          } catch (...) {
          }
        } catch (...) {
          try {
            Logger("icon-theme").warn("failed to check icon theme");
          } catch (...) {
          }
        }
        {
          std::scoped_lock lock(state->mutex);
          if (state->stopping) {
            return;
          }
          state->undeliveredChanged |= changed;
        }
        try {
          DeferredCall::callLater([this, alive, state]() {
            if (alive.expired()) {
              return;
            }
            bool changed;
            {
              std::scoped_lock lock(state->mutex);
              state->pending = false;
              changed = std::exchange(state->undeliveredChanged, false);
            }
            if (changed && m_changeCallback) {
              m_changeCallback();
            }
          });
        } catch (...) {
          {
            std::scoped_lock lock(state->mutex);
            state->pending = false;
          }
          // Preserve a real theme change until a later interval can notify UI.
          constexpr char message[] = "noctalia icon-theme: cannot publish check; later interval may retry\n";
          (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        }
      }
    });
  }

  ~IconThemePollSource() override {
    m_alive.reset();
    {
      std::scoped_lock lock(m_state->mutex);
      m_state->stopping = true;
    }
    m_state->cv.notify_one();
    // The one owned worker is joined; in-flight kernel filesystem IO may delay
    // shutdown. Never detach a task that can publish into destroyed UI state.
    m_worker.join();
  }

  void setChangeCallback(std::function<void()> callback) { m_changeCallback = std::move(callback); }

  [[nodiscard]] int pollTimeoutMs() const override {
    const auto now = Clock::now();
    if (now >= m_nextCheck) {
      return 0;
    }
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(m_nextCheck - now).count());
  }

  void dispatch(const std::vector<pollfd>& /*fds*/, std::size_t /*startIdx*/) override {
    const auto now = Clock::now();
    if (now < m_nextCheck) {
      return;
    }

    m_nextCheck = now + m_interval;
    {
      std::scoped_lock lock(m_state->mutex);
      if (m_state->pending) {
        return;
      }
      m_state->pending = true;
      m_state->requested = true;
    }
    m_state->cv.notify_one();
  }

protected:
  void doAddPollFds(std::vector<pollfd>& /*fds*/) override {}

private:
  std::function<void()> m_changeCallback;
  std::chrono::milliseconds m_interval;
  Clock::time_point m_nextCheck;
  std::shared_ptr<State> m_state = std::make_shared<State>();
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  std::thread m_worker;
};
