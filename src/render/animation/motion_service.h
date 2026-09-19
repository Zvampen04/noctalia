#pragma once

#include "ui/signal.h"
#include "render/animation/motion_curve.h"
#include <cstdint>
#include <unordered_set>

class AnimationManager;

class MotionService {
public:
  static MotionService& instance();

  MotionService(const MotionService&) = delete;
  MotionService& operator=(const MotionService&) = delete;

  void registerManager(AnimationManager* manager);
  void unregisterManager(AnimationManager* manager);

  void setEnabled(bool enabled);
  void setSpeed(float speed);
  void setStyle(MotionStyle style, MotionCurve curve = {});

  // Manual control drivers pass their own native curve result exactly once.
  [[nodiscard]] float easedProgress(float progress, float nativeValue) const noexcept;
  [[nodiscard]] MotionStyle style() const noexcept { return m_style; }
  [[nodiscard]] const MotionCurve& curve() const noexcept { return m_curve; }

  [[nodiscard]] Signal<>& changed() noexcept { return m_changed; }

  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] float speed() const noexcept { return m_speed; }

private:
  MotionService() = default;

  Signal<> m_changed;
  bool m_enabled = true;
  float m_speed = 1.0F;
  MotionStyle m_style = MotionStyle::Native;
  MotionCurve m_curve;
  std::unordered_set<AnimationManager*> m_managers;
};
