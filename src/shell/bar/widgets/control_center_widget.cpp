#include "shell/bar/widgets/control_center_widget.h"
#include "shell/bar/widgets/system_update_status.h"

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
    refreshSystemUpdateState();
    if (m_fileWatcher != nullptr) {
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
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  m_updateState = m_updateTracker.update(status, session, prompt, now);
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
        colorSpecFromConfigString(std::string(system_update_status::color(m_updateState)), "system updates")
    );
  }
}
