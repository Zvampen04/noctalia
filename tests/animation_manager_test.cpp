#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"

#include <cmath>
#include <iostream>

namespace {

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

  bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.0001F; }

  void resetMotion() {
    auto& motion = MotionService::instance();
    motion.setSpeed(1.0F);
    motion.setEnabled(true);
    motion.setStyle(MotionStyle::Native);
  }

} // namespace

int main() {
  bool ok = true;
  resetMotion();
  {
    auto& motion = MotionService::instance();
    auto now = std::chrono::steady_clock::time_point{};
    AnimationManager manager([&] { return now; });
    float visual = -1, driver = -1, timer = -1;
    const auto start = [&] {
      manager.cancelAll();
      now = {};
      visual = driver = timer = -1;
      manager.animate(0, 1, 1000, Easing::EaseOutQuad, [&](float v) { visual = v; });
      manager.animateProgress(0, 1, 1000, [&](float v) { driver = v; });
      manager.animateTimer(0, 1, 1000, Easing::Linear, [&](float v) { timer = v; });
    };
    for (const auto style : {MotionStyle::Native, MotionStyle::Expressive, MotionStyle::Linear, MotionStyle::Custom}) {
      motion.setStyle(style, {0.2F, -1.0F, 0.7F, 2.0F});
      start();
      now += std::chrono::milliseconds(250);
      manager.tick(0);
      const float expected = style == MotionStyle::Native ? applyEasing(Easing::EaseOutQuad, .25F)
          : style == MotionStyle::Linear ? .25F
          : applyMotionCurve(.25F, style == MotionStyle::Expressive ? MotionCurve{} : motion.curve());
      ok &= check(nearlyEqual(visual, expected), "visual curve precedence is incorrect");
      ok &= check(nearlyEqual(driver, .25F), "manual progress was globally eased twice");
      ok &= check(nearlyEqual(timer, .25F), "timer driver was changed by visual curve");
      ok &= check(nearlyEqual(motion.easedProgress(driver, applyEasing(Easing::EaseOutQuad, driver)), expected),
                  "manual control precedence differs from manager curve");
    }
    motion.setSpeed(2);
    motion.setStyle(MotionStyle::Linear);
    start();
    now += std::chrono::milliseconds(250);
    manager.tick(0);
    ok &= check(nearlyEqual(visual, .5F) && nearlyEqual(driver, .5F) && nearlyEqual(timer, .25F),
                "speed must affect visual/progress drivers only");
    motion.setEnabled(false);
    ok &= check(nearlyEqual(visual, 1) && nearlyEqual(driver, 1) && nearlyEqual(timer, .25F),
                "None must settle visual/progress drivers without settling timers");
    now += std::chrono::milliseconds(250);
    manager.tick(0);
    ok &= check(nearlyEqual(timer, .5F), "timer deadline stopped while motion was disabled");
    now += std::chrono::milliseconds(500);
    manager.tick(0);
    ok &= check(nearlyEqual(timer, 1) && !manager.hasActive(), "timer did not finish at its original deadline");
  }
  resetMotion();
  {
    float previous = 0;
    for (int i = 0; i <= 1000; ++i) {
      const float t = static_cast<float>(i) / 1000;
      const float value = applyMotionCurve(t, MotionCurve{});
      ok &= check(value >= previous && value >= 0 && value <= 1, "Expressive curve overshoots or reverses");
      previous = value;
      ok &= check(nearlyEqual(applyMotionCurve(t, {0, 0, 1, 1}), t), "custom linear Bezier is inaccurate");
    }
    ok &= check(applyMotionCurve(.25F, {0, -2, 1, -2}) < 0, "custom negative Y was discarded");
    ok &= check(applyMotionCurve(.75F, {0, 2, 1, 2}) > 1, "custom overshoot Y was discarded");
    ok &= check(applyMotionCurve(0, {0, -2, 1, 2}) == 0 && applyMotionCurve(1, {0, -2, 1, 2}) == 1,
                "custom endpoints must remain exact");
  }

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = 0.0F;
    bool completed = false;
    const auto id = manager.animate(
        0.0F, 10.0F, 200.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "reduced-motion animation with completion should remain cancellable");
    ok &= check(nearlyEqual(value, 10.0F), "reduced-motion animation did not snap to target");
    ok &= check(!completed, "reduced-motion completion ran synchronously");

    manager.tick(0.0F);

    ok &= check(completed, "reduced-motion completion did not run on tick");
    ok &= check(!manager.hasActive(), "reduced-motion animation stayed active after completion");
  }

  resetMotion();

  {
    AnimationManager manager;

    float value = 0.0F;
    bool completed = false;
    const auto id = manager.animate(
        0.0F, 10.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "normal animation did not start");

    MotionService::instance().setEnabled(false);

    ok &= check(nearlyEqual(value, 10.0F), "active animation did not snap when motion was disabled");
    ok &= check(!completed, "active animation completion ran synchronously when motion was disabled");
    ok &= check(manager.hasActive(), "active animation did not remain pending for async completion");

    manager.tick(0.0F);

    ok &= check(completed, "active animation completion did not run after reduced-motion snap");
  }

  resetMotion();

  {
    AnimationManager manager;
    MotionService::instance().setEnabled(false);

    float value = -1.0F;
    bool completed = false;
    const auto id = manager.animateTimer(
        1.0F, 0.0F, 1000.0F, Easing::Linear, [&value](float v) { value = v; }, [&completed]() { completed = true; }
    );

    ok &= check(id != 0, "timer animation did not start while motion was disabled");
    ok &= check(nearlyEqual(value, -1.0F), "timer animation snapped when motion was disabled");

    manager.tick(0.0F);

    ok &= check(!completed, "timer animation completed early while motion was disabled");
    ok &= check(manager.hasActive(), "timer animation was removed early while motion was disabled");
    manager.cancel(id);
  }

  resetMotion();
  return ok ? 0 : 1;
}
