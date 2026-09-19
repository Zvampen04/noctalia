#pragma once

#include "config/config_types.h"
#include "shell/surface/shadow.h"
#include "wayland/surface.h"

#include <cstdint>
#include <vector>

class Box;
class Node;
class PopupSurface;
class RectNode;
struct PopupSurfaceConfig;

namespace popup_chrome {

  enum class HorizontalAttachment : std::uint8_t {
    Left,
    Center,
    Right,
  };

  enum class VerticalAttachment : std::uint8_t {
    Top,
    Center,
    Bottom,
  };

  struct Attachment {
    HorizontalAttachment horizontal = HorizontalAttachment::Center;
    VerticalAttachment vertical = VerticalAttachment::Top;
  };

  struct Geometry {
    shell::surface_shadow::Bleed bleed{};
    float contentWidth = 1.0F;
    float contentHeight = 1.0F;
    std::uint32_t surfaceWidth = 1;
    std::uint32_t surfaceHeight = 1;

    [[nodiscard]] float contentX() const noexcept { return static_cast<float>(bleed.left); }
    [[nodiscard]] float contentY() const noexcept { return static_cast<float>(bleed.up); }
    [[nodiscard]] float contentRight() const noexcept { return contentX() + contentWidth; }
    [[nodiscard]] float contentBottom() const noexcept { return contentY() + contentHeight; }
    [[nodiscard]] InputRect inputRect() const noexcept;
  };

  [[nodiscard]] Geometry computeGeometry(
      float contentWidth, float contentHeight, const ShellConfig::ShadowConfig& shadow, bool componentShadow = true,
      std::string_view materialSurface = {}, std::string_view materialFamily = "container"
  ) noexcept;
  [[nodiscard]] std::int32_t
  adjustedOffsetX(std::int32_t baseOffset, const Geometry& geometry, HorizontalAttachment attachment) noexcept;
  [[nodiscard]] std::int32_t
  adjustedOffsetY(std::int32_t baseOffset, const Geometry& geometry, VerticalAttachment attachment) noexcept;

  void applyToConfig(PopupSurfaceConfig& config, const Geometry& geometry, Attachment attachment) noexcept;
  [[nodiscard]] std::vector<InputRect>
  roundedContentRegion(const Geometry& geometry, float radius, float cornerPower = 2.0F);
  void setContentInputRegion(PopupSurface& surface, const Geometry& geometry, float radius, float cornerPower);
  [[nodiscard]] RectNode* addShadow(
      Node& parent, const Geometry& geometry, const ShellConfig::ShadowConfig& shadow, float radius,
      float backgroundOpacity = 1.0F
  );
  // Rounded popup card background at the fixed content rect. Hosts that scroll a
  // ContextMenuControl draw the card here so its corners stay pinned to the viewport
  // instead of scrolling away with the rows.
  Box* addCardBackground(Node& parent, const Geometry& geometry, float contentScale);

} // namespace popup_chrome
