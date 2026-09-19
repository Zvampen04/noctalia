#include "shell/bar/widgets/control_center_widget.h"

#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "render/scene/countdown_ring_node.h"
#include "dbus/network/inetwork_service.h"
#include "dbus/network/network_display.h"
#include "dbus/upower/upower_service.h"
#include "core/files/file_watcher.h"
#include "system/system_monitor_service.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <memory>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
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
    wl_output* /*output*/, Options options, INetworkService* network, UPowerService* upower,
    SystemMonitorService* sysmon, FileWatcher* fileWatcher
)
    : m_barGlyphId(std::move(options.glyph)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)),
      m_showRing(options.ring), m_iconSize(static_cast<float>(options.iconSize)), m_iconSource(options.iconSource),
      m_ringSource(options.ringSource), m_network(network), m_upower(upower), m_sysmon(sysmon),
      m_fileWatcher(fileWatcher) {
  if (m_sysmon != nullptr && m_showRing && m_ringSource == RingSource::Gpu) m_sysmon->retainGpuUsage();
}

ControlCenterWidget::~ControlCenterWidget() {
  if (m_sysmon != nullptr && m_showRing && m_ringSource == RingSource::Gpu) m_sysmon->releaseGpuUsage();
  if (m_fileWatcher != nullptr)
    for (const auto id : m_updateWatchIds) if (id != 0) m_fileWatcher->unwatch(id);
}

void ControlCenterWidget::create() {
  auto area = ui::inputArea({});
  if (m_showRing) {
    auto ring = std::make_unique<CountdownRingNode>();
    m_ring = ring.get(); ring->setHitTestVisible(false); ring->setParticipatesInLayout(false);
    ring->setThickness(1.2F * m_contentScale);
    area->addChild(std::move(ring));
  }

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
  const auto state = status.value("status", std::string{});
  const auto phase = status.value("phase", std::string{});
  const bool settled = state == "success"
      && (phase == "up-to-date" || phase == "no-changes" || phase == "switched" || phase == "completed");
  const bool attention = state == "failed" || sourceRequiresReboot(status) || sourceRequiresReboot(session)
      || !prompt.empty() || (state == "success" && (phase == "staged" || phase == "scheduled"))
      || hasActionableItems(status, settled) || (!status.contains("items") && hasActionableItems(session, settled));
  m_updateState = attention ? UpdateState::Attention : state == "success" ? UpdateState::Current : UpdateState::Unknown;
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
  if (m_ring) {
    const float diameter = std::max(node->width(), node->height()) + 12.0F * m_contentScale;
    const float contentWidth = node->width();
    const float contentHeight = node->height();
    node->setSize(diameter, diameter);
    if (m_image != nullptr)
      m_image->setPosition((diameter - contentWidth) * 0.5F, (diameter - contentHeight) * 0.5F);
    if (m_glyph != nullptr)
      m_glyph->setPosition((diameter - contentWidth) * 0.5F, (diameter - contentHeight) * 0.5F);
    m_ring->setPosition(0.0F, 0.0F);
    m_ring->setSize(diameter, diameter);
    m_ring->setColor(colorForRole(ColorRole::Primary));
  }
  doUpdate(renderer);
}

void ControlCenterWidget::doUpdate(Renderer& /*renderer*/) {
  if (m_iconSource == IconSource::Wifi && m_network != nullptr && m_glyph != nullptr)
    m_glyph->setGlyph(network_display::wifiGlyphForState(m_network->state()));
  if (m_iconSource == IconSource::SystemUpdates && m_glyph != nullptr) {
    m_glyph->setGlyph("refresh");
    const auto role = m_updateState == UpdateState::Attention ? ColorRole::Error
        : m_updateState == UpdateState::Current ? ColorRole::Primary : ColorRole::OnSurface;
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(role)));
  }
  if (!m_showRing) return;
  if (m_ring) {
    float progress = 0.0F;
    bool critical = false;
    if (m_ringSource == RingSource::Battery) {
      const auto state = m_upower ? m_upower->state() : UPowerState{};
      progress = state.isPresent ? static_cast<float>(state.percentage / 100.0) : 0.0F;
      critical = state.isPresent && state.percentage <= 15.0;
    } else if (m_sysmon != nullptr) {
      const auto stats = m_sysmon->latest();
      switch (m_ringSource) {
      case RingSource::Ram: progress = static_cast<float>(stats.ramUsagePercent / 100.0); break;
      case RingSource::Cpu: progress = static_cast<float>(stats.cpuUsagePercent / 100.0); break;
      case RingSource::Gpu: progress = static_cast<float>(stats.gpuUsagePercent.value_or(0.0) / 100.0); break;
      case RingSource::Battery: break;
      }
    }
    if (!std::isfinite(progress)) progress = 0.0F;
    m_ring->setProgress(std::clamp(progress, 0.0F, 1.0F));
    m_ring->setColor(colorForRole(critical ? ColorRole::Error : ColorRole::Primary));
  }
}
