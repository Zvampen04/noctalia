#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"
#include "shell/bar/widgets/system_update_status.h"

#include <cstdint>
#include <array>
#include <string>

class Glyph;
class CountdownRingNode;
class FileWatcher;
class INetworkService;
class UPowerService;
class Image;
class SystemMonitorService;
struct wl_output;

class ControlCenterWidget : public Widget {
public:
  enum class IconSource : std::uint8_t { Static, Wifi, SystemUpdates };
  enum class RingSource : std::uint8_t { Battery, Ram, Cpu, Gpu, UpdateProgress };

  struct Options {
    std::string glyph = "noctalia";
    std::string customImage;
    bool customImageColorize = false;
    bool ring = false;
    std::int32_t iconSize = 0;
    IconSource iconSource = IconSource::Static;
    RingSource ringSource = RingSource::Battery;
  };

  ControlCenterWidget(
      wl_output* output, Options options, INetworkService* network = nullptr, UPowerService* upower = nullptr,
      SystemMonitorService* sysmon = nullptr, FileWatcher* fileWatcher = nullptr
  );
  ~ControlCenterWidget() override;

  [[nodiscard]] static constexpr bool consumesSystemUpdates(IconSource source, bool hasGlyph) noexcept {
    return source == IconSource::SystemUpdates && hasGlyph;
  }

  void create() override;

private:
  void doUpdate(Renderer& renderer) override;
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  void refreshSystemUpdateState();
  std::string m_barGlyphId;
  WidgetCustomImage m_customImage;
  float m_iconSize = 0;
  IconSource m_iconSource = IconSource::Static;
  INetworkService* m_network = nullptr;
  FileWatcher* m_fileWatcher = nullptr;
  std::array<std::uint64_t, 3> m_updateWatchIds{};
  system_update_status::Tracker m_updateTracker;
  system_update_status::State m_updateState = system_update_status::State::Unknown;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
};
