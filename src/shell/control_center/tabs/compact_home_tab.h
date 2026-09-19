#pragma once
#include "shell/control_center/control_center_services.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/tabs/media_tab.h"
#include "shell/control_center/tabs/notifications_tab.h"
#include "ui/material_target_catalog.h"
#include <vector>

class Slider;
class Glyph;
class Button;
class Shortcut;
class CompactHomeTab : public Tab {
public:
  explicit CompactHomeTab(const ControlCenterServices& services);
  ~CompactHomeTab() override;
  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
private:
  void doLayout(Renderer&, float width, float height) override;
  void doUpdate(Renderer&) override;
  ControlCenterServices m_services;
  MediaTab m_media;
  NotificationsTab m_notifications;
  Flex* m_root = nullptr;
  Flex* m_top = nullptr;
  Flex* m_connections = nullptr;
  Flex* m_mediaRoot = nullptr;
  Flex* m_notificationRoot = nullptr;
  Flex* m_brightnessCard = nullptr;
  struct Action {
    std::unique_ptr<Shortcut> shortcut;
    Button* button = nullptr;
    Style::MaterialTargetRegistration materialRegistration;
  };
  std::vector<Action> m_actions;
  std::vector<Style::MaterialTargetRegistration> m_materialRegistrations;
  Label* m_wifiDetail = nullptr;
  Label* m_bluetoothDetail = nullptr;
  Button* m_wifi = nullptr;
  Button* m_bluetooth = nullptr;
  Slider* m_brightness = nullptr;
  Slider* m_volume = nullptr;
  Glyph* m_brightnessGlyph = nullptr;
  Glyph* m_volumeGlyph = nullptr;
  bool m_syncing = false;
  bool m_showMedia = true;
};
