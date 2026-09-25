#include "compositors/hyprland/material_bridge.h"
#include "config/atomic_file.h"
#include "config/schema/config_schema.h"
#include "core/toml.h"
#include "util/file_utils.h"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace compositors::hyprland {
namespace {
void updateTerminalAppearance(const Config& config) {
  const auto state = FileUtils::stateDir();
  if (state.empty()) return;
  const auto path = std::filesystem::path(state) / "caret-apps/terminal-ghostty.conf";
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (!error && std::filesystem::is_symlink(status)) return;
  const auto& terminal = config.shell.terminalAppearance;
  if (!terminal.enabled) {
    if (!error && std::filesystem::is_regular_file(status)) {
      std::ifstream input(path);
      std::string firstLine;
      std::getline(input, firstLine);
      if (firstLine == "# Noctalia terminal surface; text and palette stay independent.")
        std::filesystem::remove(path, error);
    }
    return;
  }
  std::ostringstream value;
  value.imbue(std::locale::classic());
  value << "# Noctalia terminal surface; text and palette stay independent.\n"
      << "background-opacity = " << std::fixed << std::setprecision(6)
      << terminal.backgroundOpacity << '\n'
      << "background-blur = " << (terminal.backgroundBlur ? "true" : "false") << '\n'
      << "background-opacity-cells = " << (terminal.opacityCells ? "true" : "false") << '\n';
  if (terminal.paddingX >= 0) value << "window-padding-x = " << terminal.paddingX << '\n';
  if (terminal.paddingY >= 0) value << "window-padding-y = " << terminal.paddingY << '\n';
  if (terminal.fontSize > 0) value << "font-size = " << std::setprecision(2) << terminal.fontSize << '\n';
  if (!error && std::filesystem::is_regular_file(status)) {
    std::ifstream input(path);
    if (std::string(std::istreambuf_iterator<char>(input), {}) == value.str()) return;
  }
  (void)writeTextFileAtomic(path, value.str(),
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
}
}
struct MaterialBridge::Impl {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<std::string> pending;
  std::string last;
  bool stopping = false;
  std::thread worker;
  Impl() : worker([this] { run(); }) {}
  ~Impl() {
    { std::lock_guard guard(mutex); stopping = true; }
    ready.notify_one();
    worker.join();
  }
  bool transmit(const std::string& command) {
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
      if (!runtime || !signature) return false;
      const std::string path = std::string(runtime) + "/hypr/" + signature + "/.socket.sock";
      sockaddr_un address{}; address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) return false;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) return false;
      const timeval timeout{0, 100000};
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      bool sent = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
      std::size_t offset = 0;
      while (sent && offset < command.size()) {
        const auto count = send(fd, command.data() + offset, command.size() - offset, MSG_NOSIGNAL);
        if (count <= 0) { sent = false; break; }
        offset += static_cast<std::size_t>(count);
      }
      shutdown(fd, SHUT_WR);
      char reply[4096];
      const auto count = sent ? recv(fd, reply, sizeof(reply), 0) : -1;
      close(fd);
      return count > 0 && std::string_view(reply, count).find("ok") != std::string_view::npos;
  }
  void run() {
    for (;;) {
      std::string command;
      {
        std::unique_lock guard(mutex);
        ready.wait(guard, [this] { return stopping || pending.has_value(); });
        if (stopping) return;
        command = std::move(*pending); pending.reset();
      }
      if (!transmit(command)) {
        std::lock_guard guard(mutex);
        if (last == command) last.clear();
        continue;
      }
      // Hyprset restores compositor settings shortly after session startup.
      // Reassert the current shell material once after that replay settles.
      std::unique_lock guard(mutex);
      if (ready.wait_for(guard, std::chrono::seconds(2), [this] { return stopping || pending.has_value(); })) continue;
      if (last != command) continue;
      guard.unlock();
      (void)transmit(command);
    }
  }
};
MaterialBridge::MaterialBridge() = default;
MaterialBridge::~MaterialBridge() = default;
void MaterialBridge::update(const Config& config) {
  updateTerminalAppearance(config);
  if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE") ||
      !config.plugins.pluginSettings.contains("infinite-desktop/settings")) return;
  if (!m_impl) m_impl = std::make_unique<Impl>();
  toml::table shell;
  for (const auto& field : noctalia::config::schema::shellSchema())
    if (field.key == "material" || field.key == "material_overrides" || field.key == "surface_material")
      field.write(shell, config.shell);
  std::ostringstream serialized; serialized << toml::json_formatter{shell};
  const auto values = nlohmann::json::parse(serialized.str());
  const auto overrides = values.value("material_overrides", nlohmann::json::object());
  const nlohmann::json parameters = {{"global", values.at("material")},
      {"roles", overrides.value("roles", nlohmann::json::object())},
      {"families", overrides.value("families", nlohmann::json::object())},
      {"surfaces", overrides.value("surfaces", nlohmann::json::object())}};
  const nlohmann::json request = {{"op", "set"}, {"values", {
      {"theme.material", values.at("surface_material")}, {"theme.parameters", parameters.dump()}}}};
  const std::string command = "eval hl.plugin.canvasviewport.settings(" + nlohmann::json(request.dump()).dump() + ")";
  { std::lock_guard guard(m_impl->mutex);
    if (command == m_impl->last) return;
    m_impl->last = command; m_impl->pending = command;
  }
  m_impl->ready.notify_one();
}
}
