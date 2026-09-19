#pragma once

#include "ui/control_settings.h"
#include <algorithm>
#include <cmath>
#include <optional>

struct SegmentedIndicatorBounds {
  float x = 0, y = 0, width = 0, height = 0;
  bool operator==(const SegmentedIndicatorBounds&) const = default;
};

inline SegmentedIndicatorBounds boundSegmentedIndicator(SegmentedIndicatorBounds value, float width, float height) {
  const float right = std::clamp(value.x + value.width, 0.0F, std::max(0.0F, width));
  const float bottom = std::clamp(value.y + value.height, 0.0F, std::max(0.0F, height));
  value.x = std::clamp(value.x, 0.0F, std::max(0.0F, width));
  value.y = std::clamp(value.y, 0.0F, std::max(0.0F, height));
  value.width = std::max(0.0F, right - value.x);
  value.height = std::max(0.0F, bottom - value.y);
  return value;
}

inline SegmentedIndicatorBounds segmentedIndicatorTarget(
    SegmentedIndicatorBounds option, float inset, float width, float height) {
  const float horizontal = std::min(inset, option.width * 0.5F);
  const float vertical = std::min(inset, option.height * 0.5F);
  return boundSegmentedIndicator({option.x + horizontal, option.y + vertical,
      std::max(0.0F, option.width - 2 * horizontal), std::max(0.0F, option.height - 2 * vertical)}, width, height);
}

// Retained geometry model used by the control and CPU tests. Retargeting starts
// from the actual painted geometry, including any interrupted travel stretch.
class SegmentedIndicatorTransition {
public:
  [[nodiscard]] bool valid() const noexcept { return m_valid; }
  [[nodiscard]] const SegmentedIndicatorBounds& current() const noexcept { return m_current; }
  [[nodiscard]] const SegmentedIndicatorBounds& target() const noexcept { return m_target; }
  void clear() { m_valid = false; m_from = m_current = m_target = {}; }
  void snap(SegmentedIndicatorBounds target) { m_valid = true; m_from = m_current = m_target = target; }
  bool retarget(SegmentedIndicatorBounds target) {
    if (!m_valid) { snap(target); return false; }
    m_from = m_current;
    m_target = target;
    return m_from != m_target;
  }
  void advance(float progress, const Style::ControlSettings& controls, float width, float height,
               std::optional<float> resolvedProgress = {}) {
    const float raw = std::clamp(progress, 0.0F, 1.0F);
    const float t = resolvedProgress.value_or(Style::controlBezier(raw, controls.segmented_curve_x1, controls.segmented_curve_y1,
                                       controls.segmented_curve_x2, controls.segmented_curve_y2));
    const auto mix = [t](float from, float to) { return from + (to - from) * t; };
    SegmentedIndicatorBounds frame{mix(m_from.x, m_target.x), mix(m_from.y, m_target.y),
        mix(m_from.width, m_target.width), mix(m_from.height, m_target.height)};
    if (raw > 0 && raw < 1 && m_from.x != m_target.x) {
      const float stretch = std::max(0.0F, frame.width) * controls.segmented_travel_stretch * std::sin(raw * 3.14159265358979323846F);
      if (m_target.x < m_from.x) frame.x -= stretch;
      frame.width += stretch;
    }
    m_current = boundSegmentedIndicator(frame, width, height);
    if (raw == 1) m_current = m_target;
  }
private:
  SegmentedIndicatorBounds m_from, m_current, m_target;
  bool m_valid = false;
};
