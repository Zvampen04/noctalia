#include "shell/bar/widgets/control_center_widget.h"

#include "core/files/file_watcher.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "dbus/upower/upower_service.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>

namespace {
  constexpr std::array<std::string_view, 3> kSystemUpdatePaths{
      "/var/lib/nixos-auto-update-switch/status.json",
      "/var/lib/nixos-auto-update-switch/item-session.json",
      "/var/lib/nixos-auto-update-switch/apply-prompt.json",
  };

  nlohmann::json readJsonFile(std::string_view path) {
    std::ifstream input{std::string(path)};
    if (!input) return nlohmann::json::object();
    try { return nlohmann::json::parse(input); } catch (...) { return nlohmann::json::object(); }
  }

  bool sourceRequiresReboot(const nlohmann::json& source) {
    return source.is_object() && (source.value("reboot_required", false)
        || (source.contains("activation") && source["activation"].is_object()
            && source["activation"].value("reboot_required", false)));
  }

  bool hasActionableItems(const nlohmann::json& source, bool settled) {
    if (!source.contains("items") || !source["items"].is_array()) return false;
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    for (const auto& item : source["items"]) {
      if (!item.is_object()) continue;
      const auto state = item.value("status", std::string{});
      if (state == "deferred" && item.value("deferred_until_epoch", std::int64_t{now + 1}) <= now) return true;
      if (!settled && (state == "available" || state == "selected" || state == "accepted" || state == "staged"))
        return true;
    }
    return false;
  }
}

ControlCenterWidget::ControlCenterWidget(
    wl_output* /*output*/, Options options, INetworkService* network, UPowerService* /*upower*/,
    SystemMonitorService* /*sysmon*/, FileWatcher* fileWatcher
)
    : m_barGlyphId(std::move(options.glyph)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)),
      m_iconSize(static_cast<float>(options.iconSize)), m_iconSource(options.iconSource), m_network(network),
      m_fileWatcher(fileWatcher) {}

ControlCenterWidget::~ControlCenterWidget() {
  if (m_fileWatcher != nullptr)
    for (const auto id : m_updateWatchIds) if (id != 0) m_fileWatcher->unwatch(id);
}

void ControlCenterWidget::create() {
  auto area = ui::inputArea({});

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_barGlyphId,
            .glyphSize = (m_iconSize > 0 ? m_iconSize : Style::baseGlyphSize) * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
        })
    );
  }

  setRoot(std::move(area));
  if (consumesSystemUpdates(m_iconSource, m_glyph != nullptr)) {
    const auto* cache = std::getenv("XDG_CACHE_HOME");
    const auto* home = std::getenv("HOME");
    m_updateColorsPath = std::string(
                             cache && *cache ? cache
                                 : home      ? std::string(home) + "/.cache"
                                             : "/tmp"
                         )
        + "/noctalia/system-updates-colors.json";
    refreshSystemUpdateState();
    if (m_fileWatcher != nullptr) {
      m_updateWatchIds[3] = m_fileWatcher->watch(
          m_updateColorsPath,
          [this] {
            refreshSystemUpdateState();
            requestUpdate();
          },
          FileWatcher::WatchTrigger::WriteCompleted
      );
      for (std::size_t i = 0; i < kSystemUpdatePaths.size(); ++i) {
        m_updateWatchIds[i] = m_fileWatcher->watch(
            std::filesystem::path(kSystemUpdatePaths[i]), [this] { refreshSystemUpdateState(); requestUpdate(); },
            FileWatcher::WatchTrigger::WriteCompleted
        );
      }
    }
  }
}

void ControlCenterWidget::refreshSystemUpdateState() {
  const auto status = readJsonFile(kSystemUpdatePaths[0]);
  const auto session = readJsonFile(kSystemUpdatePaths[1]);
  const auto prompt = readJsonFile(kSystemUpdatePaths[2]);
  const auto colors = readJsonFile(m_updateColorsPath);
  if (colors.contains("success") && colors["success"].is_string())
    m_updateSuccessColor = colorSpecFromConfigString(colors["success"].get<std::string>(), "system-update success");
  const auto state = status.value("status", std::string{});
  const auto phase = status.value("phase", std::string{});
  const bool settled = state == "success"
      && (phase == "up-to-date" || phase == "no-changes" || phase == "switched" || phase == "completed");
  const bool attention = state == "failed" || sourceRequiresReboot(status) || sourceRequiresReboot(session)
      || !prompt.empty() || (state == "success" && (phase == "staged" || phase == "scheduled"))
      || hasActionableItems(status, settled) || (!status.contains("items") && hasActionableItems(session, settled));
  m_updateState = state == "running" ? UpdateState::Unknown : attention ? UpdateState::Attention : state == "success" ? UpdateState::Current : UpdateState::Unknown;
}

void ControlCenterWidget::doLayout(Renderer& renderer, float /*containerWidth*/, float /*containerHeight*/) {
  auto* node = root();
  if (node == nullptr) {
    return;
  }

  if (m_image != nullptr) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
    node->setSize(m_image->width(), m_image->height());
  } else if (m_glyph != nullptr) {
    m_glyph->setGlyphSize((m_iconSize > 0 ? m_iconSize : Style::baseGlyphSize) * m_contentScale);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
    node->setSize(m_glyph->width(), m_glyph->height());
  }
  doUpdate(renderer);
}

void ControlCenterWidget::doUpdate(Renderer& /*renderer*/) {
  if (m_iconSource == IconSource::Wifi && m_network != nullptr && m_glyph != nullptr)
    m_glyph->setGlyph(network_display::wifiGlyphForState(m_network->state()));
  if (m_iconSource == IconSource::SystemUpdates && m_glyph != nullptr) {
    m_glyph->setGlyph("refresh");
    // Dynamic status colors must not be replaced by a static capsule foreground.
    m_glyph->setColor(
        m_updateState == UpdateState::Current
            ? m_updateSuccessColor
            : colorSpecFromRole(m_updateState == UpdateState::Attention ? ColorRole::Error : ColorRole::OnSurface)
    );
  }
}
