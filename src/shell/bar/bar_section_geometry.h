#pragma once

#include <algorithm>
#include <array>

namespace bar_sections {

struct Extent {
  float start = 0;
  float end = 0;
  bool visible = false;
};

enum class Alignment { Start, Center, End };

[[nodiscard]] inline float alignedStart(
    float start, float span, float length, Alignment alignment) {
  span = std::max(0.0F, span);
  length = std::clamp(length, 0.0F, span);
  const float free = span - length;
  switch (alignment) {
    case Alignment::Start: return start;
    case Alignment::Center: return start + free * 0.5F;
    case Alignment::End: return start + free;
  }
  return start;
}

[[nodiscard]] inline std::array<Extent, 2> equidistantEdgeExtents(
    float start, float span, float startLength, float endLength) {
  span = std::max(0.0F, span);
  const float lane = span / 3.0F;
  startLength = std::clamp(startLength, 0.0F, lane);
  endLength = std::clamp(endLength, 0.0F, lane);
  const float firstCenter = start + lane * 0.5F;
  const float lastCenter = start + span - lane * 0.5F;
  return {{
      {std::max(start, firstCenter - startLength * 0.5F),
       std::min(start + span, firstCenter + startLength * 0.5F), startLength > 0.0F},
      {std::max(start, lastCenter - endLength * 0.5F),
       std::min(start + span, lastCenter + endLength * 0.5F), endLength > 0.0F},
  }};
}

[[nodiscard]] inline Extent clippedExtent(float slotStart, float slotEnd,
                                           float contentStart, float contentEnd, bool visible) {
  const float start = std::max(slotStart, contentStart);
  const float end = std::min(slotEnd, contentEnd);
  return {start, std::max(start, end), visible && end > start};
}

// Extents are the actual, already slot-clipped content in start/center/end order.
// Add end padding without joining neighbouring surfaces, even on a narrow output.
[[nodiscard]] inline std::array<Extent, 3> paddedExtents(
    std::array<Extent, 3> content, float span, float padding, float gap) {
  span = std::max(0.0F, span);
  padding = std::max(0.0F, padding);
  gap = std::max(0.0F, gap);
  auto result = content;
  for (std::size_t i = 0; i < content.size(); ++i) {
    content[i].start = std::clamp(content[i].start, 0.0F, span);
    content[i].end = std::clamp(content[i].end, content[i].start, span);
    content[i].visible = content[i].visible && content[i].end > content[i].start;
    result[i] = {std::max(0.0F, content[i].start - padding),
                 std::min(span, content[i].end + padding), content[i].visible};
  }
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (!content[i].visible) continue;
    for (std::size_t j = i + 1; j < content.size(); ++j) {
      if (!content[j].visible) continue;
      const float available = std::max(0.0F, content[j].start - content[i].end);
      const float halfGap = std::min(gap, available) * 0.5F;
      const float midpoint = (content[i].end + content[j].start) * 0.5F;
      result[i].end = std::min(result[i].end, midpoint - halfGap);
      result[j].start = std::max(result[j].start, midpoint + halfGap);
      break;
    }
  }
  return result;
}

struct Rect { float x = 0, y = 0, width = 0, height = 0; };
[[nodiscard]] inline Rect rectangle(Extent extent, float cross, bool vertical) {
  const float length = extent.visible ? std::max(0.0F, extent.end - extent.start) : 0.0F;
  return vertical ? Rect{0, extent.start, std::max(0.0F, cross), length}
                  : Rect{extent.start, 0, length, std::max(0.0F, cross)};
}

} // namespace bar_sections
