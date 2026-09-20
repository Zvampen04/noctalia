#include "shell/control_center/tabs/compact_home_tab.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/timer_manager.h"
#include "dbus/bluetooth/bluetooth_service.h"
#include "dbus/network/inetwork_service.h"
#include "pipewire/pipewire_service.h"
#include "shell/bar/bar_material_target.h"
#include "shell/bar/widgets/tray_widget.h"
#include "shell/control_center/shortcut_identity.h"
#include "shell/control_center/shortcut_registry.h"
#include "shell/panel/panel_manager.h"
#include "system/brightness_service.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/glyph.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"

#include <algorithm>
#include <unordered_map>

namespace {
void openSection(const char* context) {
  DeferredCall::callLater([context] { PanelManager::instance().openPanel("control-center", {.context = context}); });
}

bool hasEnabledBarMedia(const Config& config, const WaylandOutput* output) {
  const auto isMedia = [&config](const std::string& name) {
    const auto configured = config.widgets.find(name);
    if (configured == config.widgets.end()) return name == "media";
    return configured->second.type == "media" && configured->second.getBool("enabled", true);
  };
  return std::ranges::any_of(config.bars, [&isMedia, output](const BarConfig& configuredBar) {
    const BarConfig bar = output != nullptr
        ? ConfigService::resolveForOutput(configuredBar, *output)
        : configuredBar;
    if (!bar.enabled) return false;
    std::unordered_map<std::string, std::string> placements;
    for (const auto& placement : bar.widgetPlacements)
      placements.insert_or_assign(placement.id, placement.widget);
    const auto entryIsMedia = [&isMedia, &placements](const std::string& entry) {
      return isMedia(std::string(noctalia::bar::resolveBarWidgetLaneEntry(entry, placements).widgetConfigName));
    };
    if (!bar.sections.empty()) {
      return std::ranges::any_of(bar.sections, [&entryIsMedia](const BarSectionConfig& section) {
        return std::ranges::any_of(section.widgets, entryIsMedia);
      });
    }
    return std::ranges::any_of(bar.startWidgets, entryIsMedia)
        || std::ranges::any_of(bar.centerWidgets, entryIsMedia)
        || std::ranges::any_of(bar.endWidgets, entryIsMedia);
  });
}
}

CompactHomeTab::CompactHomeTab(const ControlCenterServices& services)
    : m_services(services),
      m_media(services.mpris, services.httpClient, nullptr, services.config,
          services.platform ? &services.platform->wayland() : nullptr, PanelManager::instance().renderContext()),
      m_notifications(services.notifications, services.platform) {
  m_media.setCompactMini(true);
  m_notifications.setEmbedded(true);
}

CompactHomeTab::~CompactHomeTab() = default;

std::unique_ptr<Flex> CompactHomeTab::create() {
  const float s = contentScale();
  m_materialRegistrations.clear();
  if (m_services.config != nullptr) {
    const auto& config = m_services.config->config();
    switch (config.controlCenter.media.homeVisibility) {
    case ControlCenterMediaHomeVisibility::Always:
      m_showMedia = true;
      break;
    case ControlCenterMediaHomeVisibility::Hidden:
      m_showMedia = false;
      break;
    case ControlCenterMediaHomeVisibility::Auto:
      m_showMedia = !hasEnabledBarMedia(
          config,
          m_services.platform != nullptr
              ? m_services.platform->findOutputByWl(PanelManager::instance().attachedPanelOutput())
              : nullptr
      );
      break;
    }
  }
  m_media.setContentScale(s);
  m_media.setPanelCardOpacity(panelCardOpacity());
  m_notifications.setContentScale(s);
  m_notifications.setPanelCardOpacity(panelCardOpacity());
  auto root = ui::column({.out = &m_root, .align = FlexAlign::Stretch, .gap = 12.0F * s});
  auto top = ui::row({.out = &m_top, .align = FlexAlign::Stretch, .gap = 12.0F * s,
      .minHeight = 132.0F * s, .maxHeight = 132.0F * s});
  auto connections = ui::column({.out = &m_connections, .align = FlexAlign::Stretch, .gap = 12.0F * s});
  auto tile = [&](const char* title, const char* icon, const char* context, std::string targetId, Button** toggle,
      Label** detail, std::function<void()> action) {
    auto row = ui::row({.align = FlexAlign::Center, .gap = 8.0F * s, .padding = 9.0F * s,
        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()), .radius = 30.0F * s,
        .minHeight = 60.0F * s, .maxHeight = 60.0F * s});
    const auto path = std::vector<std::string>{"panel", "control-center", "control-center.home",
        "control-center.home.compact", "control-center.home.compact.tile", targetId};
    row->setMaterialIdentityPath("surface", "card", path);
    m_materialRegistrations.push_back(Style::MaterialTargetCatalog::instance().registerInstance(
        control_center_material::descriptor(targetId, std::string("Compact Quick Settings: ") + title, "card", path)));
    row->addChild(ui::button({.out = toggle, .glyph = icon, .glyphSize = 20.0F * s,
        .controlHeight = 42.0F * s, .variant = ButtonVariant::Primary,
        .tooltip = title, .minWidth = 42.0F * s, .maxWidth = 42.0F * s,
        .padding = 0.0F, .radius = 21.0F * s, .onClick = std::move(action)}));
    auto labels = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .flexGrow = 1.0F});
    labels->addChild(ui::button({.text = title, .fontSize = 14.0F * s, .controlHeight = 21.0F * s,
        .contentAlign = ButtonContentAlign::Start, .variant = ButtonVariant::Ghost,
        .padding = 0.0F, .onClick = [context] { openSection(context); }}));
    labels->addChild(ui::label({.out = detail, .fontSize = 10.0F * s,
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant), .maxLines = 1}));
    row->addChild(std::move(labels));
    return row;
  };
  connections->addChild(tile("Wi-Fi", "wifi", "network", "control-center.home.compact.wifi", &m_wifi, &m_wifiDetail, [this] {
    if (m_services.network) m_services.network->setWirelessEnabled(!m_services.network->state().wirelessEnabled);
  }));
  connections->addChild(tile("Bluetooth", "bluetooth", "bluetooth", "control-center.home.compact.bluetooth", &m_bluetooth, &m_bluetoothDetail, [this] {
    if (m_services.bluetooth) m_services.bluetooth->setPowered(!m_services.bluetooth->state().powered);
  }));
  connections->setFlexGrow(1.0F);
  top->addChild(std::move(connections));
  if (m_showMedia) {
    auto media = m_media.create();
    m_mediaRoot = media.get();
    media->setFlexGrow(1.0F);
    top->addChild(std::move(media));
  }
  root->addChild(std::move(top));
  auto sliderCard = [&](const char* title, const char* context, std::string targetId, Slider** slider,
                        std::function<void(double)> change) {
    auto card = ui::column({.align = FlexAlign::Stretch, .gap = 0.0F, .paddingV = 5.0F * s,
        .paddingH = 9.0F * s, .fill = colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()),
        .radius = 19.0F * s, .minHeight = 59.0F * s, .maxHeight = 59.0F * s});
    const auto path = std::vector<std::string>{"panel", "control-center", "control-center.home",
        "control-center.home.compact", "control-center.home.compact.slider-card", targetId};
    card->setMaterialIdentityPath("surface", "card", path);
    m_materialRegistrations.push_back(Style::MaterialTargetCatalog::instance().registerInstance(
        control_center_material::descriptor(targetId, std::string("Compact Quick Settings: ") + title, "card", path)));
    card->addChild(ui::row({.align = FlexAlign::Center, .justify = FlexJustify::SpaceBetween, .paddingH = 5.0F * s},
        ui::label({.text = title, .fontSize = 13.0F * s, .color = colorSpecFromRole(ColorRole::OnSurface)}),
        ui::button({.glyph = "chevron-right", .glyphSize = 14.0F * s, .controlHeight = 22.0F * s,
            .variant = ButtonVariant::Ghost, .padding = 0.0F, .onClick = [context] { openSection(context); }})));
    card->addChild(ui::slider({.out = slider, .minValue = 0.0, .maxValue = 100.0, .step = 1.0,
        .trackHeight = 24.0F * s, .thumbSize = 0.0F, .controlHeight = 24.0F * s,
        .onValueChanged = std::move(change)}));
    (*slider)->addChild(ui::glyph({.out = std::string(context) == "monitor" ? &m_brightnessGlyph : &m_volumeGlyph, .glyph = std::string(context) == "monitor" ? "sun" : "volume",
        .glyphSize = 14.F * s, .color = colorSpecFromRole(ColorRole::OnPrimary),
        .configure = [s](Glyph& icon) { icon.setParticipatesInLayout(false); icon.setHitTestVisible(false); icon.setPosition(10.F * s, 5.F * s); }}));
    return card;
  };
  auto brightnessCard = sliderCard("Display", "monitor", "control-center.home.compact.brightness", &m_brightness, [this](double v) {
    if (!m_syncing && m_services.brightness) m_services.brightness->setAllBrightness(static_cast<float>(v / 100.0));
  });
  m_brightnessCard = brightnessCard.get();
  root->addChild(std::move(brightnessCard));
  root->addChild(sliderCard("Sound", "audio", "control-center.home.compact.volume", &m_volume, [this](double v) {
    if (!m_syncing && m_services.audio) m_services.audio->setVolume(static_cast<float>(v / 100.0));
  }));
  auto notifications = ui::column({.align = FlexAlign::Stretch, .gap = 4.0F * s, .paddingV = 8.0F * s, .paddingH = 14.0F * s,
      .fill = colorSpecFromRole(ColorRole::SurfaceVariant, panelCardOpacity()), .radius = 19.0F * s, .flexGrow = 1.0F});
  notifications->addChild(ui::button({.text = "Notifications", .fontSize = 13.0F * s,
      .controlHeight = 22.0F * s, .contentAlign = ButtonContentAlign::Start, .variant = ButtonVariant::Ghost,
      .padding = 0.0F, .onClick = [] { openSection("notifications"); }}));
  auto list = m_notifications.create(); m_notificationRoot = list.get(); list->setFlexGrow(1.0F);
  notifications->addChild(std::move(list)); root->addChild(std::move(notifications));

  m_tray.reset();
  if (m_services.tray && m_services.config && m_services.config->config().controlCenter.showTray) {
    auto viewport = std::make_unique<ScrollView>();
    viewport->setOrientation(ScrollOrientation::Horizontal);
    viewport->setScrollbarVisible(false);
    viewport->setContentScale(s);
    viewport->setMinHeight(36.0F * s);
    viewport->setMaxHeight(36.0F * s);
    m_tray = std::make_unique<TrayWidget>(
        *m_services.config, m_services.tray, TrayWidget::Options{.hidePassive = false, .inlineEntryGap = 12.0F}
    );
    m_tray->setContentScale(s);
    m_tray->create();
    viewport->content()->addChild(m_tray->releaseRoot());
    root->addChild(std::move(viewport));
  }

  auto actionsViewport = std::make_unique<ScrollView>();
  actionsViewport->setOrientation(ScrollOrientation::Horizontal);
  actionsViewport->setScrollbarVisible(false);
  actionsViewport->setContentScale(s);
  actionsViewport->setViewportPaddingH(0.0F);
  actionsViewport->setViewportPaddingV(4.0F * s);
  actionsViewport->setMinHeight(58.0F * s);
  actionsViewport->setMaxHeight(58.0F * s);
  auto* actions = actionsViewport->content();
  actions->setDirection(FlexDirection::Horizontal);
  actions->setAlign(FlexAlign::Center);
  actions->setGap(8.0F * s);
  actions->setPadding(0.0F);
  m_actions.clear();
  const auto& configured = m_services.config != nullptr
      ? m_services.config->config().controlCenter.shortcuts : std::vector<ShortcutConfig>{};
  std::unordered_map<std::string, std::size_t> shortcutTotals;
  std::unordered_map<std::string, std::size_t> shortcutOrdinals;
  for (const auto& shortcut : configured) ++shortcutTotals[shortcut.type];
  for (std::size_t configuredIndex = 0; configuredIndex < configured.size(); ++configuredIndex) {
    const auto& entry = configured[configuredIndex];
    auto shortcut = ShortcutRegistry::create(entry.type, m_services.shortcutServices());
    if (shortcut == nullptr) continue;
    shortcut->onPanelOpen();
    const std::size_t index = m_actions.size();
    auto button = ui::button({
        .glyph = shortcut->displayIcon(), .glyphSize = 20.0F * s,
        .controlHeight = 46.0F * s, .tooltip = shortcut->displayLabel(),
        .minWidth = 46.0F * s, .maxWidth = 46.0F * s, .padding = 0.0F,
        .radius = 23.0F * s,
        .onClick = [this, index] {
          if (index < m_actions.size() && m_actions[index].shortcut) m_actions[index].shortcut->onClick();
        },
        .onRightClick = [this, index] {
          if (index < m_actions.size() && m_actions[index].shortcut) m_actions[index].shortcut->onRightClick();
        }});
    auto* buttonPtr = button.get();
    const std::size_t shortcutOrdinal = ++shortcutOrdinals[entry.type];
    const std::string materialLabel = "Compact shortcut: " + shortcut->displayLabel()
        + (shortcutTotals[entry.type] > 1 ? " (" + std::to_string(shortcutOrdinal) + ")" : "");
    const std::string baseTarget = control_center_material::shortcutTargetId(entry, configuredIndex);
    const std::string target = "control-center.home.compact.shortcut."
        + baseTarget.substr(baseTarget.find_last_of('.') + 1);
    const auto path = std::vector<std::string>{"panel", "control-center", "control-center.home",
        "control-center.home.compact", "control-center.home.compact.shortcut", target};
    button->setMaterialIdentityPath("surface", "button", path);
    auto registration = Style::MaterialTargetCatalog::instance().registerInstance(
        control_center_material::descriptor(target, materialLabel, "button", path));
    m_actions.push_back({.shortcut = std::move(shortcut), .button = buttonPtr,
                         .materialRegistration = std::move(registration)});
    actions->addChild(std::move(button));
  }
  if (!m_actions.empty()) root->addChild(std::move(actionsViewport));
  return root;
}

void CompactHomeTab::doLayout(Renderer& renderer, float width, float height) {
  if (!m_root) return;
  m_root->setSize(width, height);
  const float leftWidth = m_showMedia
      ? std::max(1.0F, (width - 12.0F * contentScale()) * .425F)
      : std::max(1.0F, width);
  m_connections->setMinWidth(leftWidth); m_connections->setMaxWidth(leftWidth);
  for (auto* label : {m_wifiDetail, m_bluetoothDetail}) label->setMaxWidth(std::max(1.0F, leftWidth - 68.0F * contentScale()));
  if (m_tray)
    m_tray->layout(renderer, width, 36.0F * contentScale());
  m_root->layout(renderer);
  for (auto* icon : {m_brightnessGlyph, m_volumeGlyph}) if (icon) icon->layout(renderer);
  if (m_mediaRoot != nullptr) m_media.layout(renderer, m_mediaRoot->width(), m_mediaRoot->height());
  m_notifications.layout(renderer, m_notificationRoot->width(), m_notificationRoot->height());
}

void CompactHomeTab::doUpdate(Renderer& renderer) {
  if (!m_root) return;
  m_syncing = true;
  if (m_services.network) {
    const auto& n = m_services.network->state();
    m_wifi->setVariant(n.wirelessEnabled ? ButtonVariant::Primary : ButtonVariant::Default);
    m_wifiDetail->setText(n.connected ? (n.ssid.empty() ? n.interfaceName : n.ssid) : "Not connected");
  } else { m_wifi->setEnabled(false); m_wifiDetail->setText("Unavailable"); }
  if (m_services.bluetooth) {
    const auto& b = m_services.bluetooth->state(); m_bluetooth->setEnabled(b.adapterPresent);
    m_bluetooth->setVariant(b.powered ? ButtonVariant::Primary : ButtonVariant::Default);
    std::string text = b.powered ? "Not connected" : "Off";
    for (const auto& device : m_services.bluetooth->devices()) if (device.connected) { text = device.alias; break; }
    m_bluetoothDetail->setText(text);
  } else { m_bluetooth->setEnabled(false); m_bluetoothDetail->setText("Unavailable"); }
  const auto* sink = m_services.audio ? m_services.audio->defaultSink() : nullptr;
  m_volume->setEnabled(sink != nullptr);
  if (sink && !m_volume->dragging()) m_volume->setValue(sink->volume * 100.0);
  const auto* display = m_services.brightness && !m_services.brightness->displays().empty()
      ? &m_services.brightness->displays().front() : nullptr;
  const bool hasBrightness = display != nullptr && display->controllable;
  if (m_brightnessCard != nullptr) {
    m_brightnessCard->setVisible(hasBrightness);
    m_brightnessCard->setParticipatesInLayout(hasBrightness);
  }
  m_brightness->setEnabled(display && display->controllable);
  if (display && !m_brightness->dragging()) m_brightness->setValue(display->brightness * 100.0);
  for (auto& action : m_actions) {
    if (!action.shortcut || !action.button) continue;
    action.button->setEnabled(action.shortcut->enabled());
    action.button->setVariant(action.shortcut->isToggle() && action.shortcut->active()
        ? ButtonVariant::Primary : ButtonVariant::Default);
    if (auto* glyph = action.button->glyph()) glyph->setGlyph(action.shortcut->displayIcon());
  }
  m_syncing = false;
  if (m_mediaRoot != nullptr) m_media.update(renderer);
  m_notifications.update(renderer);
  if (m_tray)
    m_tray->update(renderer);
}
void CompactHomeTab::setActive(bool active) {
  if (m_showMedia) m_media.setActive(active);
  m_notifications.setActive(active);
}
void CompactHomeTab::onFrameTick(float dt) {
  if (m_showMedia) m_media.onFrameTick(dt);
  m_notifications.onFrameTick(dt);
}
void CompactHomeTab::onClose() {
  m_tray.reset();
  m_brightnessGlyph = nullptr; m_volumeGlyph = nullptr;
  if (m_showMedia) m_media.onClose();
  m_notifications.onClose();
  for (auto& action : m_actions) if (action.shortcut) action.shortcut->onPanelClose();
  m_actions.clear();
  m_materialRegistrations.clear();
  m_root = m_top = m_connections = m_mediaRoot = m_notificationRoot = m_brightnessCard = nullptr;
  m_wifiDetail = m_bluetoothDetail = nullptr; m_wifi = m_bluetooth = nullptr; m_brightness = m_volume = nullptr;
}
