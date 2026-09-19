#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace bar_island_morph {

struct Extent {
  float start = 0.0F;
  float end = 0.0F;
  bool visible = false;
  bool operator==(const Extent&) const = default;
};

struct Result {
  std::array<Extent, 3> extents{};
  bool fitsSurface = true;
  float overflowBefore = 0.0F;
  float overflowAfter = 0.0F;
};

struct Sides {
  float left = 0.0F;
  float top = 0.0F;
  float right = 0.0F;
  float bottom = 0.0F;
};

struct MainInsets {
  float start = 0.0F;
  float end = 0.0F;
  bool operator==(const MainInsets&) const = default;
};

// A margin-trimmed layer surface places the visual body at
// layerMargin + localInset. A stable full-main-axis surface must fold both
// terms into local geometry to leave the compact bar at the same output
// coordinates, including concave logical insets.
[[nodiscard]] inline MainInsets stableSurfaceMainInsets(
    std::string_view position, float marginEnds, Sides shadowBleed, Sides logicalInset
) {
  const bool vertical = position == "left" || position == "right";
  const float startBleed = vertical ? shadowBleed.top : shadowBleed.left;
  const float endBleed = vertical ? shadowBleed.bottom : shadowBleed.right;
  const float startLogical = vertical ? logicalInset.top : logicalInset.left;
  const float endLogical = vertical ? logicalInset.bottom : logicalInset.right;
  const auto fold = [marginEnds](float bleed, float inset) {
    return std::max(0.0F, marginEnds - bleed - inset)
        + std::min(marginEnds, bleed) + inset;
  };
  return {fold(startBleed, startLogical), fold(endBleed, endLogical)};
}

// Expands one section toward the panel-reported interval. Neighbouring sections
// retain their widths and move only as far as needed to preserve minimumGap.
// Coordinates remain in the bar surface's main axis so the active section stays
// aligned with the independently rendered attached panel. Surface overflow is
// reported to the caller; translating the result would break that seam.
[[nodiscard]] inline Result reflow(
    std::array<Extent, 3> baseline, std::size_t active, Extent target,
    float progress, float surfaceSpan, float minimumGap
) {
  Result result{.extents = baseline};
  if (active >= baseline.size() || !baseline[active].visible || !target.visible) return result;

  progress = std::clamp(progress, 0.0F, 1.0F);
  surfaceSpan = std::max(0.0F, surfaceSpan);
  minimumGap = std::max(0.0F, minimumGap);
  if (progress <= 0.0F) return result;

  if (target.end < target.start) std::swap(target.start, target.end);
  auto& expanded = result.extents[active];
  // A panel narrower than the opener must not shrink and clip retained bar
  // content. Island morphing expands around at least the compact source.
  if (target.end - target.start < expanded.end - expanded.start) {
    target.start = std::min(target.start, expanded.start);
    target.end = std::max(target.end, expanded.end);
  }
  expanded.start += (target.start - expanded.start) * progress;
  expanded.end += (target.end - expanded.end) * progress;

  const auto requiredGap = [&](std::size_t left, std::size_t right) {
    const float compactGap = std::max(0.0F, baseline[right].start - baseline[left].end);
    const float compactRequirement = std::min(compactGap, minimumGap);
    if (left != active && right != active) return compactRequirement;
    return compactRequirement + (minimumGap - compactRequirement) * progress;
  };

  float nextStart = expanded.start;
  std::size_t nextIndex = active;
  for (std::size_t offset = active; offset > 0; --offset) {
    const std::size_t siblingIndex = offset - 1;
    auto& sibling = result.extents[siblingIndex];
    if (!sibling.visible) continue;
    const float maximumEnd = nextStart - requiredGap(siblingIndex, nextIndex);
    if (sibling.end > maximumEnd) {
      const float delta = maximumEnd - sibling.end;
      sibling.start += delta;
      sibling.end += delta;
    }
    nextStart = sibling.start;
    nextIndex = siblingIndex;
  }

  float previousEnd = expanded.end;
  std::size_t previousIndex = active;
  for (std::size_t index = active + 1; index < result.extents.size(); ++index) {
    auto& sibling = result.extents[index];
    if (!sibling.visible) continue;
    const float minimumStart = previousEnd + requiredGap(previousIndex, index);
    if (sibling.start < minimumStart) {
      const float delta = minimumStart - sibling.start;
      sibling.start += delta;
      sibling.end += delta;
    }
    previousEnd = sibling.end;
    previousIndex = index;
  }

  bool found = false;
  float first = 0.0F;
  float last = 0.0F;
  for (const auto& extent : result.extents) {
    if (!extent.visible) continue;
    if (!found) {
      first = extent.start;
      last = extent.end;
      found = true;
    } else {
      first = std::min(first, extent.start);
      last = std::max(last, extent.end);
    }
  }
  result.overflowBefore = found ? std::max(0.0F, -first) : 0.0F;
  result.overflowAfter = found ? std::max(0.0F, last - surfaceSpan) : 0.0F;
  result.fitsSurface = result.overflowBefore == 0.0F && result.overflowAfter == 0.0F;
  return result;
}

} // namespace bar_island_morph
