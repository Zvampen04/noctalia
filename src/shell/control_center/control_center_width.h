#pragma once

#include "config/config_types.h"

#include <algorithm>

namespace control_center_width {

struct MediaControlsLayout {
  float sideButton = 0.0F;
  float playPauseButton = 0.0F;
  float gap = 0.0F;
  float scale = 1.0F;

  [[nodiscard]] float totalWidth() const noexcept { return sideButton * 4.0F + playPauseButton + gap * 4.0F; }
};

[[nodiscard]] inline float preferred(const ControlCenterConfig& config,
    ControlCenterSidebarMode sidebar, float contentScale = 1.0F) noexcept {
  float factor = 1.0F;
  if (!config.literalWidth) {
    if (sidebar == ControlCenterSidebarMode::None) factor = 0.75F;
    else if (sidebar == ControlCenterSidebarMode::Compact) factor = 0.85F;
  }
  const float logicalWidth = std::max(
      static_cast<float>(noctalia::config::kControlCenterMinimumWidth), static_cast<float>(config.width) * factor);
  return logicalWidth * contentScale;
}

[[nodiscard]] inline float fittedFullSidebar(float naturalWidth, float panelWidth,
    float minimumSidebarWidth, float minimumBodyWidth, float panelGap) noexcept {
  const float maximumSidebarWidth =
      std::max(minimumSidebarWidth, panelWidth - minimumBodyWidth - panelGap);
  return std::min(maximumSidebarWidth, std::max(minimumSidebarWidth, naturalWidth));
}

[[nodiscard]] inline MediaControlsLayout fittedMediaControls(
    float availableWidth, float sideButton, float playPauseButton, float gap) noexcept {
  const float naturalWidth = sideButton * 4.0F + playPauseButton + gap * 4.0F;
  const float fitScale = naturalWidth > 0.0F ? std::min(1.0F, std::max(0.0F, availableWidth) / naturalWidth) : 1.0F;
  return {.sideButton = sideButton * fitScale,
      .playPauseButton = playPauseButton * fitScale,
      .gap = gap * fitScale,
      .scale = fitScale};
}

} // namespace control_center_width
