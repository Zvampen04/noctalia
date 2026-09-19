#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace bezier_editor {
using Curve = std::array<float, 4>;
struct Point { float x = 0, y = 0; };
struct Viewport { float x = 0, y = 0, width = 0, height = 0; };

[[nodiscard]] inline bool normalize(Curve& curve) {
  for (const auto value : curve) if (!std::isfinite(value)) return false;
  for (std::size_t i = 0; i < curve.size(); ++i)
    curve[i] = std::clamp(curve[i], i % 2 == 0 ? 0.0F : -2.0F, i % 2 == 0 ? 1.0F : 2.0F);
  return true;
}
[[nodiscard]] inline Point evaluate(const Curve& curve, float t) {
  t = std::clamp(t, 0.0F, 1.0F);
  const float u = 1 - t;
  return {3*u*u*t*curve[0] + 3*u*t*t*curve[2] + t*t*t,
          3*u*u*t*curve[1] + 3*u*t*t*curve[3] + t*t*t};
}
[[nodiscard]] inline Point toScreen(Point value, Viewport view) {
  return {view.x + value.x * view.width, view.y + (2 - value.y) * view.height * 0.25F};
}
[[nodiscard]] inline Point fromScreen(Point value, Viewport view) {
  return {view.width > 0 ? std::clamp((value.x - view.x) / view.width, 0.0F, 1.0F) : 0.0F,
          view.height > 0 ? std::clamp(2 - 4 * (value.y - view.y) / view.height, -2.0F, 2.0F) : 0.0F};
}
[[nodiscard]] inline bool moveHandle(Curve& curve, std::size_t handle, Point point) {
  if (handle > 1 || !std::isfinite(point.x) || !std::isfinite(point.y)) return false;
  auto next = curve;
  next[handle*2] = point.x;
  next[handle*2+1] = point.y;
  if (!normalize(next) || next == curve) return false;
  curve = next;
  return true;
}
[[nodiscard]] inline std::array<Point, 65> polyline(const Curve& curve, Viewport view) {
  std::array<Point, 65> points{};
  for (std::size_t i = 0; i < points.size(); ++i)
    points[i] = toScreen(evaluate(curve, static_cast<float>(i) / 64), view);
  return points;
}
} // namespace bezier_editor
