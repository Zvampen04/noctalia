#pragma once

#include "render/animation/animation.h"

#include <cstdint>
#include <functional>
#include <vector>

class AnimationManager {
public:
  using Id = std::uint32_t;
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  explicit AnimationManager(Clock clock = std::chrono::steady_clock::now);
  ~AnimationManager();

  AnimationManager(const AnimationManager&) = delete;
  AnimationManager& operator=(const AnimationManager&) = delete;
  AnimationManager(AnimationManager&&) = delete;
  AnimationManager& operator=(AnimationManager&&) = delete;

  Id animate(
      float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr
  );
  // Real elapsed-time driver: ignores global motion enable/speed. Use for timeouts,
  // never use this bypass for decorative visual effects.
  Id animateTimer(
      float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr
  );
  // Visual elapsed progress for controls that resolve easing themselves. This
  // still obeys motion enable/speed; it is not a deadline/timer bypass.
  Id animateProgress(
      float from, float to, float durationMs, std::function<void(float)> setter,
      std::function<void()> onComplete = {}, const void* owner = nullptr
  );
  void cancel(Id id);
  void cancelAll();
  void reduceMotion();
  // Cancels any animations tagged with the given owner. Called from Node's destructor so that
  // animations holding a raw pointer to a scene node can never outlive their target.
  void cancelForOwner(const void* owner);
  void tick(float deltaMs);
  [[nodiscard]] bool hasActive() const;

private:
  struct Entry {
    Id id = 0;
    const void* owner = nullptr;
    bool respectMotionEnabled = true;
    bool resolveMotionCurve = true;
    Animation animation;
  };

  std::vector<Entry> m_animations;
  Id m_nextId = 1;
  Clock m_clock;

  Id animateInternal(
      float from, float to, float durationMs, Easing easing, std::function<void(float)> setter,
      std::function<void()> onComplete, const void* owner, bool scaleDuration, bool respectMotionEnabled,
      bool resolveMotionCurve
  );
};
