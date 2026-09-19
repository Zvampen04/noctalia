#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

enum class MotionStyle : std::uint8_t { Native, Expressive, Linear, Custom };

struct MotionCurve {
  float x1 = 0.34F;
  float y1 = 0.8F;
  float x2 = 0.34F;
  float y2 = 1.0F;

  bool operator==(const MotionCurve&) const = default;
};

inline std::optional<MotionStyle> parseMotionStyle(std::string_view value) {
  if (value == "native") return MotionStyle::Native;
  if (value == "expressive") return MotionStyle::Expressive;
  if (value == "linear") return MotionStyle::Linear;
  if (value == "custom") return MotionStyle::Custom;
  return std::nullopt;
}

inline bool validMotionCurve(const MotionCurve& curve) {
  return std::isfinite(curve.x1) && std::isfinite(curve.y1) && std::isfinite(curve.x2) && std::isfinite(curve.y2)
      && curve.x1 >= 0 && curve.x1 <= 1 && curve.x2 >= 0 && curve.x2 <= 1
      && curve.y1 >= -2 && curve.y1 <= 2 && curve.y2 >= -2 && curve.y2 <= 2;
}

// Solve x(u)=elapsed progress before evaluating y(u). Fixed bounded work also
// handles flat derivatives at legal endpoint control values. No frame driver.
inline float applyMotionCurve(float progress, const MotionCurve& curve) {
  if (progress <= 0) return 0;
  if (progress >= 1) return 1;
  if (!std::isfinite(progress) || !validMotionCurve(curve)) return 0;
  const auto coordinate = [](float u, float a, float b) {
    const float v = 1 - u;
    return 3 * v * v * u * a + 3 * v * u * u * b + u * u * u;
  };
  float low = 0, high = 1;
  for (int step = 0; step < 24; ++step) {
    const float u = (low + high) * 0.5F;
    if (coordinate(u, curve.x1, curve.x2) < progress) low = u;
    else high = u;
  }
  return coordinate((low + high) * 0.5F, curve.y1, curve.y2);
}
