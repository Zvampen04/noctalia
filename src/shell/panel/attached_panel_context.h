#pragma once

#include "render/core/render_styles.h"

#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>
#include <optional>

enum class AttachedRevealDirection : std::uint8_t {
  Down,
  Up,
  Right,
  Left,
};

enum class AttachedPanelSourceSection : std::uint8_t {
  Unknown,
  Start,
  Center,
  End,
};

// Output-local bounds of the retained bar island that opened a panel.
struct AttachedPanelSource {
  AttachedPanelSourceSection section = AttachedPanelSourceSection::Unknown;
  // Stable identity for configured sections. Legacy start/center/end sources
  // leave this empty and continue to use `section`.
  std::string sectionId;
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  Radii radii{};
  struct ContentOffset {
    float x = 0, y = 0;
    bool operator==(const ContentOffset&) const = default;
  };
  // Original section origin relative to the compact painted island, before
  // clipping/reflow. A clipped end lane can legitimately have a negative offset.
  std::optional<ContentOffset> contentOffset;
  std::optional<CountdownRingStyle> usageRing;

  [[nodiscard]] bool valid() const noexcept {
    return (section != AttachedPanelSourceSection::Unknown || !sectionId.empty()) && std::isfinite(x) && std::isfinite(y)
        && std::isfinite(width) && std::isfinite(height) && width > 0.0F && height > 0.0F
        && (!contentOffset || (std::isfinite(contentOffset->x) && std::isfinite(contentOffset->y)));
  }
};

struct AttachedPanelGeometry {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  // Current away-side convex radius; grows continuously during a morph.
  float cornerRadius = 0.0F;
  // Current bar-side concave radius. Morph uses the same evolving radius;
  // the selectable legacy slide can reveal this edge separately.
  float bulgeRadius = 0.0F;
  float cornerPower = 2.0F;
  // Output-local silhouette. Unlike x/y, this remains valid when a morph-enabled
  // bar uses a full-output surface instead of margin-trimmed layer geometry.
  float outputX = 0.0F;
  float outputY = 0.0F;
  float outputWidth = 0.0F;
  float outputHeight = 0.0F;
  // Output-local final silhouette. The current panel reveal can have a
  // nonzero seed width, so the bar interpolates from its compact source to
  // these final bounds using revealProgress instead of following that seed.
  float finalOutputX = 0.0F;
  float finalOutputY = 0.0F;
  float finalOutputWidth = 0.0F;
  float finalOutputHeight = 0.0F;
  float revealProgress = 0.0F;
  AttachedPanelSource source;
  // Set only after the panel surface has a painted replacement for the retained
  // source island. The bar must not hide its source before this becomes true.
  bool panelOwnsSource = false;
};

namespace attached_panel {

  [[nodiscard]] inline CornerShapes cornerShapes(std::string_view barPosition) {
    if (barPosition == "bottom") {
      return CornerShapes{
          .tl = CornerShape::Convex,
          .tr = CornerShape::Convex,
          .br = CornerShape::Concave,
          .bl = CornerShape::Concave,
      };
    }
    if (barPosition == "left") {
      return CornerShapes{
          .tl = CornerShape::Concave,
          .tr = CornerShape::Convex,
          .br = CornerShape::Convex,
          .bl = CornerShape::Concave,
      };
    }
    if (barPosition == "right") {
      return CornerShapes{
          .tl = CornerShape::Convex,
          .tr = CornerShape::Concave,
          .br = CornerShape::Concave,
          .bl = CornerShape::Convex,
      };
    }
    // top (default)
    return CornerShapes{
        .tl = CornerShape::Concave,
        .tr = CornerShape::Concave,
        .br = CornerShape::Convex,
        .bl = CornerShape::Convex,
    };
  }

  [[nodiscard]] inline RectInsets logicalInset(std::string_view barPosition, float radius) {
    const bool vertical = (barPosition == "left" || barPosition == "right");
    if (vertical) {
      return RectInsets{
          .left = 0.0F,
          .top = radius,
          .right = 0.0F,
          .bottom = radius,
      };
    }
    return RectInsets{
        .left = radius,
        .top = 0.0F,
        .right = radius,
        .bottom = 0.0F,
    };
  }

  [[nodiscard]] inline AttachedRevealDirection revealDirection(std::string_view barPosition) {
    if (barPosition == "bottom") {
      return AttachedRevealDirection::Up;
    }
    if (barPosition == "left") {
      return AttachedRevealDirection::Right;
    }
    if (barPosition == "right") {
      return AttachedRevealDirection::Left;
    }
    return AttachedRevealDirection::Down;
  }

  [[nodiscard]] inline Radii cornerRadii(std::string_view barPosition, float radius) {
    if (barPosition == "bottom") {
      return Radii{0.0F, 0.0F, radius, radius};
    }
    if (barPosition == "left") {
      return Radii{0.0F, radius, radius, 0.0F};
    }
    if (barPosition == "right") {
      return Radii{radius, 0.0F, 0.0F, radius};
    }
    return Radii{radius, radius, 0.0F, 0.0F};
  }

} // namespace attached_panel
