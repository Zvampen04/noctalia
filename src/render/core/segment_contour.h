#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

enum class SegmentContourKind : std::uint8_t {
  None = 0,
  Powerline = 1,
  PowerlineStart = 2,
  PowerlineEnd = 3,
};

struct SegmentContour {
  SegmentContourKind kind = SegmentContourKind::None;
  float depth = 0.0F;
  bool vertical = false;

  constexpr bool operator==(const SegmentContour&) const = default;
};

namespace segment_contour {

[[nodiscard]] inline bool hasLeadingCut(SegmentContourKind kind) noexcept {
  return kind == SegmentContourKind::Powerline || kind == SegmentContourKind::PowerlineEnd;
}

[[nodiscard]] inline bool hasTrailingCut(SegmentContourKind kind) noexcept {
  return kind == SegmentContourKind::Powerline || kind == SegmentContourKind::PowerlineStart;
}

[[nodiscard]] inline float clampedDepth(float mainExtent, float crossExtent, float requested) noexcept {
  if (!std::isfinite(mainExtent) || !std::isfinite(crossExtent) || !std::isfinite(requested)) return 0.0F;
  return std::clamp(requested, 0.0F, std::max(0.0F, std::min(mainExtent, crossExtent) * 0.5F));
}

// Returns the covered interval on the main axis for a cross-axis coordinate.
// Powerline cuts lean towards logical end as cross increases. Adjacent segments
// tile exactly when their layout overlap equals depth.
[[nodiscard]] inline std::pair<float, float>
mainSpan(float mainExtent, float crossExtent, float cross, SegmentContourKind kind, float requestedDepth) noexcept {
  const float depth = clampedDepth(mainExtent, crossExtent, requestedDepth);
  const float t = crossExtent > 0.0F ? std::clamp(cross / crossExtent, 0.0F, 1.0F) : 0.0F;
  const float begin = hasLeadingCut(kind) ? depth * t : 0.0F;
  const float end = mainExtent - (hasTrailingCut(kind) ? depth * (1.0F - t) : 0.0F);
  return {std::min(begin, end), std::max(begin, end)};
}

[[nodiscard]] inline bool contains(
    float width, float height, float x, float y, const SegmentContour& contour
) noexcept {
  if (contour.kind == SegmentContourKind::None) return x >= 0.0F && x < width && y >= 0.0F && y < height;
  const float main = contour.vertical ? height : width;
  const float cross = contour.vertical ? width : height;
  const float mainPoint = contour.vertical ? y : x;
  const float crossPoint = contour.vertical ? x : y;
  if (crossPoint < 0.0F || crossPoint >= cross) return false;
  const auto [begin, end] = mainSpan(main, cross, crossPoint, contour.kind, contour.depth);
  return mainPoint >= begin && mainPoint < end;
}

} // namespace segment_contour
