#pragma once

#include <algorithm>
#include <cmath>

namespace noctalia::material::shape {

inline bool validPower(float power) {
  return std::isfinite(power) && power >= 2.F && power <= 10.F;
}

inline float clampPower(float power) {
  return std::isfinite(power) ? std::clamp(power, 2.F, 10.F) : 2.F;
}

inline float clampRadius(float radius, float maximum) {
  if (!std::isfinite(radius) || !std::isfinite(maximum) || maximum <= 0.F) return 0.F;
  return std::clamp(radius, 0.F, maximum);
}

// Horizontal extent of x^p + y^p = r^p at signed offset delta.
// Normalize before exponentiation so coordinate magnitude cannot overflow the
// powered branch. This describes the contour, not Euclidean signed distance.
inline float quadrantExtent(float radius, float delta, float power = 2.F) {
  if (!std::isfinite(radius) || !std::isfinite(delta) || radius <= 0.F) return 0.F;
  const float distance = std::abs(delta);
  if (distance >= radius) return 0.F;
  power = clampPower(power);
  if (power == 2.F) {
    return std::sqrt(std::max(0.F, radius * radius - delta * delta));
  }
  const double unit = static_cast<double>(distance) / radius;
  return static_cast<float>(radius * std::pow(std::max(0.0, 1.0 - std::pow(unit, power)), 1.0 / power));
}

} // namespace noctalia::material::shape
