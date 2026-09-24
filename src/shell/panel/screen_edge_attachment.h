#pragma once

#include "config/config_types.h"
#include "shell/wallpaper/desktop_frame_geometry.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace attached_panel {

// A virtual edge gives the existing retained-panel path a stationary source
// without requiring a bar, a bar reservation, or a preset-specific renderer.
[[nodiscard]] inline std::string_view screenEdgePosition(std::string_view position) {
  // Older floating settings remain loadable after switching placement modes.
  return position == "auto" || position == "center" ? "bottom_center" : position;
}

[[nodiscard]] inline std::string screenEdge(std::string_view position) {
  position = screenEdgePosition(position);
  if (position.starts_with("bottom")) return "bottom";
  if (position.starts_with("top")) return "top";
  if (position.ends_with("left")) return "left";
  if (position.ends_with("right")) return "right";
  return "bottom";
}

[[nodiscard]] inline BarConfig screenEdgeBar(
    const ShellConfig& shell, std::string_view position, int width, int height
) {
  BarConfig edge;
  edge.name.clear();
  edge.enabled = true;
  edge.position = screenEdge(position);
  edge.marginEdge = 0;
  edge.marginEnds = 0;
  edge.padding = 0;
  edge.reserveSpace = false;
  edge.sectionBackgrounds = false;
  edge.islandMorph = false;
  edge.shadow = false;
  edge.contactShadow = false;
  // One pixel of common fill hides fractional-scale rounding at the join.
  edge.panelOverlap = 1;
  edge.radiusTopLeft = edge.radiusTopRight = edge.radiusBottomLeft = edge.radiusBottomRight = 0;
  edge.layer = shell.panel.floatingLayer;
  edge.background = shell.desktopFrame.enabled ? shell.desktopFrame.fill : colorSpecFromRole(ColorRole::Surface);
  edge.backgroundOpacity = 1.0F;
  const auto frame = desktop_frame::resolve(shell.desktopFrame, width, height);
  const float inset = !shell.desktopFrame.enabled ? 0.0F
      : edge.position == "left" ? frame.inset.left
      : edge.position == "right" ? frame.inset.right
      : edge.position == "top" ? frame.inset.top : frame.inset.bottom;
  edge.thickness = std::max(1, static_cast<int>(std::lround(inset)));
  return edge;
}

[[nodiscard]] inline float screenEdgeAnchor(
    const ShellConfig& shell, std::string_view position, int width, int height,
    float panelWidth, float panelHeight
) {
  position = screenEdgePosition(position);
  const auto frame = desktop_frame::resolve(shell.desktopFrame, width, height);
  const auto edge = screenEdge(position);
  const bool vertical = edge == "left" || edge == "right";
  const float start = !shell.desktopFrame.enabled ? 0.0F : vertical ? frame.inset.top : frame.inset.left;
  const float end = (vertical ? height : width) - (!shell.desktopFrame.enabled ? 0.0F
      : vertical ? frame.inset.bottom : frame.inset.right);
  const float extent = std::min(vertical ? panelHeight : panelWidth, std::max(0.0F, end - start));
  if ((!vertical && position.ends_with("left")) || (vertical && position.starts_with("top")))
    return start + extent * 0.5F;
  if ((!vertical && position.ends_with("right")) || (vertical && position.starts_with("bottom")))
    return end - extent * 0.5F;
  return (start + end) * 0.5F;
}

} // namespace attached_panel
