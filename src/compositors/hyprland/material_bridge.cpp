#include "compositors/hyprland/material_bridge.h"
#include "config/schema/config_schema.h"
#include "core/toml.h"
#include <condition_variable>
#include <cstdlib>
#include <cstring>
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
  void run() {
    for (;;) {
      std::string command;
      {
        std::unique_lock guard(mutex);
        ready.wait(guard, [this] { return stopping || pending.has_value(); });
        if (stopping) return;
        command = std::move(*pending); pending.reset();
      }
      const char* runtime = std::getenv("XDG_RUNTIME_DIR");
      const char* signature = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
      if (!runtime || !signature) continue;
      const std::string path = std::string(runtime) + "/hypr/" + signature + "/.socket.sock";
      sockaddr_un address{}; address.sun_family = AF_UNIX;
      if (path.size() >= sizeof(address.sun_path)) continue;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
      const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) continue;
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
      if (count <= 0 || std::string_view(reply, count).find("ok") == std::string_view::npos) {
        std::lock_guard guard(mutex);
        if (last == command) last.clear();
      }
    }
  }
};
MaterialBridge::MaterialBridge() = default;
MaterialBridge::~MaterialBridge() = default;
void MaterialBridge::update(const Config& config) {
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
