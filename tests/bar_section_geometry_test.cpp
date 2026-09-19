#include "shell/bar/bar_section_geometry.h"

#include <cassert>

int main() {
  using bar_sections::Extent;
  assert(bar_sections::alignedStart(10, 90, 30, bar_sections::Alignment::Start) == 10);
  assert(bar_sections::alignedStart(10, 90, 30, bar_sections::Alignment::Center) == 40);
  assert(bar_sections::alignedStart(10, 90, 30, bar_sections::Alignment::End) == 70);
  assert(bar_sections::alignedStart(10, 20, 30, bar_sections::Alignment::End) == 10);
  const auto equidistant = bar_sections::equidistantEdgeExtents(10, 90, 20, 40);
  assert(equidistant[0].start == 15 && equidistant[0].end == 35);
  assert(equidistant[1].start == 70 && equidistant[1].end == 100);
  const auto narrowEdges = bar_sections::equidistantEdgeExtents(0, 30, 100, 100);
  assert(narrowEdges[0].start == 0 && narrowEdges[0].end == 10);
  assert(narrowEdges[1].start == 20 && narrowEdges[1].end == 30);
  const auto normal = bar_sections::paddedExtents(
      {{{10, 80, true}, {200, 270, true}, {420, 490, true}}}, 500, 10, 8);
  assert(normal[0].start == 0 && normal[0].end == 90);
  assert(normal[1].start == 190 && normal[1].end == 280);
  assert(normal[2].start == 410 && normal[2].end == 500);
  for (const bool vertical : {false, true}) {
    const auto rect = bar_sections::rectangle(normal[1], 40, vertical);
    assert((vertical ? rect.y : rect.x) == 190);
    assert((vertical ? rect.height : rect.width) == 90);
    assert((vertical ? rect.width : rect.height) == 40);
  }
  const auto empty = bar_sections::paddedExtents({}, 500, 10, 8);
  for (const auto item : empty) assert(!item.visible);
  const auto clipped = bar_sections::paddedExtents(
      {{{-20, 45, true}, {60, 110, true}, {140, 210, true}}}, 180, 20, 8);
  assert(clipped[0].start == 0 && clipped[2].end == 180);
  assert(clipped[1].start - clipped[0].end >= 8);
  assert(clipped[2].start - clipped[1].end >= 8);
  const auto noCenter = bar_sections::paddedExtents(
      {{{10, 60, true}, {0, 0, false}, {70, 100, true}}}, 120, 20, 8);
  assert(!noCenter[1].visible && noCenter[2].start - noCenter[0].end == 8);
  const auto zero = bar_sections::paddedExtents(
      {{{10, 60, true}, {0, 0, false}, {70, 100, true}}}, 120, 0, 0);
  assert(zero[0].start == 10 && zero[0].end == 60);
  const auto narrow = bar_sections::paddedExtents(
      {{{0, 0, true}, {0, 30, true}, {30, 30, true}}}, 30, 10, 8);
  assert(!narrow[0].visible && narrow[1].visible && !narrow[2].visible);
  const auto hidden = bar_sections::rectangle(Extent{10, 60, false}, 40, false);
  assert(hidden.width == 0);
}
