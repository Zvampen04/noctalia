#pragma once

#include "render/core/color.h"
#include "render/core/frame_contour.h"
#include "render/core/segment_contour.h"
#include "material/material.h"
#include "render/custom_effect/custom_effect_types.h"

#include <array>
#include <cstdint>
#include <optional>

enum class FillMode {
  None,
  Solid,
  LinearGradient,
};

enum class GradientDirection {
  Horizontal,
  Vertical,
};

enum class CornerShape {
  Convex,
  Concave,
};

struct CornerShapes {
  CornerShape tl = CornerShape::Convex;
  CornerShape tr = CornerShape::Convex;
  CornerShape br = CornerShape::Convex;
  CornerShape bl = CornerShape::Convex;
};

constexpr bool operator==(const CornerShapes& lhs, const CornerShapes& rhs) noexcept {
  return lhs.tl == rhs.tl && lhs.tr == rhs.tr && lhs.br == rhs.br && lhs.bl == rhs.bl;
}

struct RectInsets {
  float left = 0.0F;
  float top = 0.0F;
  float right = 0.0F;
  float bottom = 0.0F;
};

constexpr bool operator==(const RectInsets& lhs, const RectInsets& rhs) noexcept {
  return lhs.left == rhs.left && lhs.top == rhs.top && lhs.right == rhs.right && lhs.bottom == rhs.bottom;
}

// Per-corner radii: top-left, top-right, bottom-right, bottom-left.
// Implicit construction from a single float sets all four corners uniformly,
// so existing `.radius = value` assignments continue to compile unchanged.
struct Radii {
  float tl = 0.0F;
  float tr = 0.0F;
  float br = 0.0F;
  float bl = 0.0F;

  Radii() = default;
  /* implicit */ Radii(float r) : tl(r), tr(r), br(r), bl(r) {} // NOLINT(google-explicit-constructor)
  Radii(float tlv, float trv, float brv, float blv) : tl(tlv), tr(trv), br(brv), bl(blv) {}
};

constexpr bool operator==(const Radii& lhs, const Radii& rhs) noexcept {
  return lhs.tl == rhs.tl && lhs.tr == rhs.tr && lhs.br == rhs.br && lhs.bl == rhs.bl;
}

struct GradientStop {
  float position = 0.0F;
  Color color{};
};

constexpr bool operator==(const GradientStop& lhs, const GradientStop& rhs) noexcept {
  return lhs.position == rhs.position && lhs.color == rhs.color;
}

// Linear-gradient darkening applied to an image's texels inside the image draw, so the image and
// its scrim resolve to a single antialiased edge instead of two stacked rounded rects. Only the
// color is affected; transparent texels stay transparent.
struct ImageScrim {
  GradientDirection direction = GradientDirection::Horizontal;
  std::array<GradientStop, 4> stops{};
  bool enabled = false;
};

constexpr bool operator==(const ImageScrim& lhs, const ImageScrim& rhs) noexcept {
  return lhs.enabled == rhs.enabled && lhs.direction == rhs.direction && lhs.stops == rhs.stops;
}

enum class MaterialBackdrop : std::uint8_t {
  Inherited, // Child control on an existing optical plane; no backdrop capture.
  Local,     // Explicit optical surface over this renderer's framebuffer.
};

struct RoundedPaintClip {
  float x=0, y=0, width=0, height=0, radius=0;
  std::optional<float> cornerPower = std::nullopt;
  constexpr bool operator==(const RoundedPaintClip&) const = default;
};

struct ContourBorderLayer {
  Color color{};
  float width = 0.0F;
  float offset = 0.0F;
  constexpr bool operator==(const ContourBorderLayer&) const = default;
};

struct RoundedRectStyle {
  Color fill{};
  Color border{};
  FillMode fillMode = FillMode::Solid;
  GradientDirection gradientDirection = GradientDirection::Horizontal;
  std::array<GradientStop, 4> gradientStops{};
  CornerShapes corners{};
  RectInsets logicalInset{};
  Radii radius{};
  std::optional<float> cornerPower = std::nullopt;
  float softness = 1.0F;
  bool noAa = false;
  bool invertFill = false;
  std::optional<FrameContour> frameContour = std::nullopt;
  std::array<ContourBorderLayer, 3> contourBorderLayers{};
  SegmentContour segmentContour{};
  float borderWidth = 0.0F;
  // Signed surface relief: positive is raised, negative is recessed. Zero is flat.
  float relief = 0.0F;
  bool liquidGlass = false;
  // Only semantic surfaces opt in. Plain/decorative rectangles remain unchanged.
  std::optional<noctalia::material::Parameters> material = std::nullopt;
  MaterialBackdrop materialBackdrop = MaterialBackdrop::Inherited;
  bool materialPlane = false; // Outer optical group published to the compositor.
  // Optional user fragment function for this element's background only. The
  // renderer keeps geometry, clipping, opacity, border and child content.
  std::optional<CustomEffectBinding> customBackground = std::nullopt;
  bool outerShadow = false;
  float shadowCutoutOffsetX = 0.0F;
  float shadowCutoutOffsetY = 0.0F;
  bool shadowExclusion = false;
  float shadowExclusionOffsetX = 0.0F;
  float shadowExclusionOffsetY = 0.0F;
  float shadowExclusionWidth = 0.0F;
  float shadowExclusionHeight = 0.0F;
  CornerShapes shadowExclusionCorners{};
  RectInsets shadowExclusionLogicalInset{};
  Radii shadowExclusionRadius{};
  std::optional<float> shadowExclusionPower = std::nullopt;
  // Coordinates are local logical pixels, transformed with the rectangle.
  std::optional<RoundedPaintClip> paintClip = std::nullopt;
};

constexpr bool operator==(const RoundedRectStyle& lhs, const RoundedRectStyle& rhs) noexcept {
  return lhs.cornerPower == rhs.cornerPower
      && lhs.shadowExclusionPower == rhs.shadowExclusionPower
      && lhs.paintClip == rhs.paintClip
      && lhs.fill == rhs.fill
      && lhs.border == rhs.border
      && lhs.fillMode == rhs.fillMode
      && lhs.gradientDirection == rhs.gradientDirection
      && lhs.corners == rhs.corners
      && lhs.gradientStops == rhs.gradientStops
      && lhs.logicalInset == rhs.logicalInset
      && lhs.radius == rhs.radius
      && lhs.softness == rhs.softness
      && lhs.noAa == rhs.noAa
      && lhs.invertFill == rhs.invertFill
      && lhs.frameContour == rhs.frameContour
      && lhs.contourBorderLayers == rhs.contourBorderLayers
      && lhs.segmentContour == rhs.segmentContour
      && lhs.borderWidth == rhs.borderWidth
      && lhs.liquidGlass == rhs.liquidGlass
      && lhs.relief == rhs.relief
      && lhs.material == rhs.material
      && lhs.materialBackdrop == rhs.materialBackdrop
      && lhs.materialPlane == rhs.materialPlane
      && lhs.customBackground == rhs.customBackground
      && lhs.outerShadow == rhs.outerShadow
      && lhs.shadowCutoutOffsetX == rhs.shadowCutoutOffsetX
      && lhs.shadowCutoutOffsetY == rhs.shadowCutoutOffsetY
      && lhs.shadowExclusion == rhs.shadowExclusion
      && lhs.shadowExclusionOffsetX == rhs.shadowExclusionOffsetX
      && lhs.shadowExclusionOffsetY == rhs.shadowExclusionOffsetY
      && lhs.shadowExclusionWidth == rhs.shadowExclusionWidth
      && lhs.shadowExclusionHeight == rhs.shadowExclusionHeight
      && lhs.shadowExclusionCorners == rhs.shadowExclusionCorners
      && lhs.shadowExclusionLogicalInset == rhs.shadowExclusionLogicalInset
      && lhs.shadowExclusionRadius == rhs.shadowExclusionRadius;
}

struct SpinnerStyle {
  Color color{};
  float thickness = 2.0F;
};

struct CountdownRingStyle {
  Color color{};
  float thickness = 6.0F;
  float progress = 1.0F;
  float radius = -1.0F; // negative uses the largest capsule radius
  bool symmetric = false;
  bool operator==(const CountdownRingStyle&) const = default;
};

enum class ScreenCornerPosition : std::uint8_t {
  TopLeft,
  TopRight,
  BottomRight,
  BottomLeft,
};

struct ScreenCornerStyle {
  Color color = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  ScreenCornerPosition position = ScreenCornerPosition::TopLeft;
  float exponent = 4.0F;
};

constexpr bool operator==(const ScreenCornerStyle& lhs, const ScreenCornerStyle& rhs) noexcept {
  return lhs.color == rhs.color && lhs.position == rhs.position && lhs.exponent == rhs.exponent;
}

enum class AudioSpectrumOrientation : std::uint8_t {
  Horizontal,
  Vertical,
};

struct AudioSpectrumStyle {
  Color color1{};
  Color color2{};
  AudioSpectrumOrientation orientation = AudioSpectrumOrientation::Horizontal;
  bool mirrored = false;
  bool reversed = false;
  bool centered = false;
  float gapRatio = 0.5F;
  float cornerRadius = 0.0F;
  float cornerPower = 2.0F;
  float reflectionHeight = 0.0F;
  float reflectionOpacity = 0.2F;
};

constexpr bool operator==(const AudioSpectrumStyle& lhs, const AudioSpectrumStyle& rhs) noexcept {
  return lhs.color1 == rhs.color1
      && lhs.color2 == rhs.color2
      && lhs.orientation == rhs.orientation
      && lhs.mirrored == rhs.mirrored
      && lhs.reversed == rhs.reversed
      && lhs.centered == rhs.centered
      && lhs.gapRatio == rhs.gapRatio
      && lhs.cornerPower == rhs.cornerPower
      && lhs.cornerRadius == rhs.cornerRadius
      && lhs.reflectionHeight == rhs.reflectionHeight
      && lhs.reflectionOpacity == rhs.reflectionOpacity;
}

enum class FancyAudioVisualizerMode : std::uint8_t {
  Bars,
  Wave,
  Rings,
  BarsRings,
  WaveRings,
  All,
};

struct FancyAudioVisualizerStyle {
  Color primaryColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  Color secondaryColor = rgba(0.0F, 0.0F, 0.0F, 1.0F);
  FancyAudioVisualizerMode mode = FancyAudioVisualizerMode::BarsRings;
  float time = 0.0F;
  float sensitivity = 1.5F;
  float rotationSpeed = 0.5F;
  float barWidth = 0.6F;
  float ringOpacity = 0.8F;
  float bloomIntensity = 0.5F;
  float waveThickness = 1.0F;
  float innerDiameter = 0.7F;
  float cornerRadius = 12.0F;
  float cornerPower = 2.0F;
};

constexpr bool operator==(const FancyAudioVisualizerStyle& lhs, const FancyAudioVisualizerStyle& rhs) noexcept {
  return lhs.primaryColor == rhs.primaryColor
      && lhs.secondaryColor == rhs.secondaryColor
      && lhs.mode == rhs.mode
      && lhs.time == rhs.time
      && lhs.sensitivity == rhs.sensitivity
      && lhs.rotationSpeed == rhs.rotationSpeed
      && lhs.barWidth == rhs.barWidth
      && lhs.ringOpacity == rhs.ringOpacity
      && lhs.bloomIntensity == rhs.bloomIntensity
      && lhs.waveThickness == rhs.waveThickness
      && lhs.innerDiameter == rhs.innerDiameter
      && lhs.cornerPower == rhs.cornerPower
      && lhs.cornerRadius == rhs.cornerRadius;
}

enum class EffectType : std::uint8_t { None, Sun, Snow, Rain, Cloud, Fog, Stars };

struct EffectStyle {
  EffectType type = EffectType::None;
  float time = 0.0F;
  float radius = 0.0F;
  Color bgColor{};
};

struct GraphStyle {
  Color lineColor1{};
  float count1 = 0.0F;
  float scroll1 = 1.0F;

  Color lineColor2{};
  float count2 = 0.0F;
  float scroll2 = 1.0F;

  Color lineColor3{};
  float count3 = 0.0F;
  float scroll3 = 1.0F;

  float lineWidth = 1.5F;
  float graphFillOpacity = 0.15F;
  float aaSize = 0.5F;
};
