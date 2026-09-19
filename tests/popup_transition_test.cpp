#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/scene/node.h"
#include "ui/popup_transition.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "popup_transition: FAIL: %s\n", message);
  std::exit(EXIT_FAILURE);
}

bool near(float left, float right) { return std::abs(left - right) < 0.0001F; }

} // namespace

int main() {
  auto& motion = MotionService::instance();
  motion.setEnabled(true);
  motion.setSpeed(1.0F);
  motion.setStyle(MotionStyle::Linear);

  auto now = std::chrono::steady_clock::time_point{};
  AnimationManager animations([&]() { return now; });
  Node paintRoot;
  paintRoot.setFrameSize(240.0F, 120.0F);
  paintRoot.setClipChildren(true);
  popup_transition::Transition transition;
  int invalidations = 0;
  int completions = 0;

  transition.open(paintRoot, animations, [&]() { ++invalidations; });
  check(transition.phase() == popup_transition::Phase::Opening && near(paintRoot.opacity(), 0.0F),
        "opening did not start hidden");
  check(paintRoot.width() == 240.0F && paintRoot.height() == 120.0F && paintRoot.clipChildren(),
        "opening changed the final input/clip footprint");
  now += std::chrono::seconds(1);
  animations.tick(0.0F);
  check(transition.phase() == popup_transition::Phase::Open && near(paintRoot.opacity(), 1.0F),
        "opening did not finish at full opacity");

  check(transition.close([&]() { ++completions; }), "close did not start");
  transition.open(paintRoot, animations, [&]() { ++invalidations; });
  now += std::chrono::seconds(1);
  animations.tick(0.0F);
  check(completions == 0 && transition.phase() == popup_transition::Phase::Open,
        "reopen allowed a stale close completion");

  check(transition.close([&]() {
    ++completions;
    transition.reset(); // A facade callback may synchronously tear down its host.
  }), "second close did not start");
  now += std::chrono::seconds(1);
  animations.tick(0.0F);
  check(completions == 1 && transition.phase() == popup_transition::Phase::Idle && near(paintRoot.opacity(), 1.0F),
        "close completion could not safely tear down/reset its host");

  motion.setEnabled(false);
  transition.open(paintRoot, animations, [&]() { ++invalidations; });
  check(near(paintRoot.opacity(), 1.0F), "motion-off opening retained a visible transition");
  animations.tick(0.0F);
  check(transition.phase() == popup_transition::Phase::Open, "motion-off opening did not settle asynchronously");
  check(transition.close([&]() { ++completions; }), "motion-off close did not start");
  check(near(paintRoot.opacity(), 0.0F) && completions == 1,
        "motion-off close did not snap without running completion inline");
  animations.tick(0.0F);
  check(completions == 2, "motion-off close completion did not run exactly once on the frame tick");
  check(invalidations > 0, "transition never invalidated its host");

  transition.reset();
  motion.setEnabled(true);
  motion.setStyle(MotionStyle::Native);
  return 0;
}
