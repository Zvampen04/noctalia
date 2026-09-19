#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "ui/controls/spinner.h"
#include <cassert>

int main() {
  auto& motion = MotionService::instance();
  motion.setEnabled(false);
  AnimationManager manager;
  Spinner spinner;
  spinner.setAnimationManager(&manager);
  spinner.start();
  assert(spinner.spinning());
  assert(!manager.hasActive());
  for (int frame = 0; frame < 120; ++frame) manager.tick(16.0F);
  assert(!manager.hasActive());
  motion.setEnabled(true);
  assert(manager.hasActive());
  manager.tick(32.0F);
  motion.setEnabled(false);
  manager.tick(0.0F);
  assert(spinner.spinning());
  assert(!manager.hasActive());
  spinner.stop();
  motion.setEnabled(true);
  assert(!manager.hasActive());
}
