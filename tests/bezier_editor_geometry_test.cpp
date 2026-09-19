#include "ui/controls/bezier_editor_geometry.h"
#include <cassert>
#include <limits>

int main() {
  using namespace bezier_editor;
  Curve curve{0.2F, 2, 0.8F, 2};
  const Viewport view{24,24,272,172};
  const auto start = evaluate(curve,0), end = evaluate(curve,1), middle = evaluate(curve,0.5F);
  assert(start.x == 0 && start.y == 0 && end.x == 1 && end.y == 1);
  assert(middle.y > 1); // Overshoot remains visible rather than clamping to the unit square.
  const auto points = polyline(curve, view);
  assert(points.size() == 65 && points.front().x == 24 && points.back().x == 296);
  assert(points[32].y < toScreen({0.5F,1},view).y);
  for (std::size_t i = 1; i < points.size(); ++i) assert(points[i].x >= points[i-1].x);
  const auto projected = toScreen({0.25F,-1},view);
  const auto restored = fromScreen(projected,view);
  assert(restored.x == 0.25F && restored.y == -1);
  assert(moveHandle(curve,0,fromScreen({-100,1000},view)));
  assert(curve[0] == 0 && curve[1] == -2);
  assert(!moveHandle(curve,0,{0,-2}));
  const auto before = curve;
  assert(!moveHandle(curve,2,{1,1}) && curve == before);
  assert(!moveHandle(curve,0,{std::numeric_limits<float>::quiet_NaN(),1}) && curve == before);
  Curve outOfRange{-4,-6,8,9};
  assert(normalize(outOfRange) && outOfRange == (Curve{0,-2,1,2}));
  const auto zero = fromScreen({1,1},{});
  assert(zero.x == 0 && zero.y == 0);
}
