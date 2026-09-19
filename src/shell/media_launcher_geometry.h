#pragma once
#include <algorithm>
#include <cmath>

namespace shell::composition {
// Both list and app-grid limits count viewport rows, never truncate results.
inline float launcherHeight(float maximum, int rows, float chrome, float cell, float gap) {
  maximum = std::max(1.0F, maximum);
  if (rows <= 0 || !std::isfinite(cell) || cell <= 0) return maximum;
  const float wanted = std::max(0.0F, chrome) + rows * cell + std::max(0, rows - 1) * std::max(0.0F, gap);
  return std::clamp(wanted, 1.0F, maximum);
}
inline float artworkSide(float availableWidth, float availableHeight, float cap) {
  const float available = std::max(1.0F, std::min(availableWidth, availableHeight));
  return cap > 0 ? std::min(available, cap) : available;
}
} // namespace shell::composition
