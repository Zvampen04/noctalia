#include "shell/bar/bar_dynamic_section_geometry.h"

#include <cassert>
#include <vector>

int main() {
  using noctalia::bar::dynamic_sections::Request;
  using noctalia::bar::dynamic_sections::resolve;

  // Four independently anchored sections exercise the collection path beyond
  // the legacy start/center/end shape. Packed sections remain ordered and keep
  // the configured gap even when their requested positions collide.
  const std::vector<Request> sections{
      {.anchor = 0.0F, .size = 15.0F, .alignment = 0.0F},
      {.anchor = 35.0F, .size = 20.0F, .alignment = 0.5F},
      {.anchor = 55.0F, .size = 18.0F, .alignment = 0.5F},
      {.anchor = 100.0F, .size = 15.0F, .alignment = 1.0F},
  };
  const auto extents = resolve(sections, 0.0F, 100.0F, 4.0F);
  assert(extents.size() == 4);
  assert(extents.front().start >= 0.0F);
  assert(extents.back().end <= 100.0F);
  for (std::size_t i = 1; i < extents.size(); ++i)
    assert(extents[i].start >= extents[i - 1].end + 4.0F);

  // Overlap is an explicit escape hatch: it retains its requested extent and
  // does not displace packed neighbours.
  auto overlapping = sections;
  overlapping[2].anchor = 50.0F;
  overlapping[2].allowOverlap = true;
  const auto withOverlap = resolve(overlapping, 0.0F, 100.0F, 4.0F);
  assert(withOverlap[2].start == 41.0F);
  assert(withOverlap[2].end == 59.0F);

  using noctalia::bar::dynamic_sections::EdgePolicy;
  using noctalia::bar::dynamic_sections::LayoutRole;
  std::vector<Request> lanes{
      {.anchor = 0, .size = 10, .offset = 2},
      {.anchor = 50, .size = 20},
      {.anchor = 100, .size = 10, .alignment = 1},
      {.anchor = 75, .size = 5},
  };
  const std::vector<LayoutRole> roles{LayoutRole::Start, LayoutRole::Center, LayoutRole::End, LayoutRole::Free};
  auto follow = lanes;
  noctalia::bar::dynamic_sections::applyLanePolicy(follow, roles, 0, 100, 4, .5F, EdgePolicy::FollowCenter);
  const auto followExtents = resolve(follow, 0, 100, 4);
  assert(followExtents[0].start == 28 && followExtents[1].start == 42 && followExtents[2].start == 66);
  assert(followExtents[3].start == 80); // free request survives policy and collision packing

  auto equal = lanes;
  noctalia::bar::dynamic_sections::applyLanePolicy(equal, roles, 0, 120, 4, .5F, EdgePolicy::Equidistant);
  const auto equalExtents = resolve(equal, 0, 120, 4);
  assert(equalExtents[0].start == 17 && equalExtents[1].start == 50 && equalExtents[2].start == 95);

  auto left = lanes;
  noctalia::bar::dynamic_sections::applyLanePolicy(left, roles, 0, 100, 4, 0.F, EdgePolicy::Edge);
  assert(left[1].anchor == 0); // exact center-lane request before collision packing
  auto right = lanes;
  noctalia::bar::dynamic_sections::applyLanePolicy(right, roles, 0, 100, 4, 1.F, EdgePolicy::Edge);
  assert(right[1].anchor == 80);
}
