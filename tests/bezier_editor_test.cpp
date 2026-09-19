#include "ui/controls/bezier_editor.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_area.h"
#include "ui/style.h"
#include "core/input/key_modifiers.h"
#include <cassert>
#include <cmath>
#include <limits>
#include <xkbcommon/xkbcommon-keysyms.h>

int main() {
  AnimationManager animations;
  BezierEditor editor;
  editor.setAnimationManager(&animations);
  int changes = 0, ends = 0;
  editor.setOnChanged([&](auto) { ++changes; });
  editor.setOnEditEnd([&] { ++ends; });
  const BezierEditor::Curve baseline{0.25F,0.5F,0.75F,0.5F};
  editor.setCurve(baseline);
  assert(changes == 0 && editor.curve() == baseline);
  editor.setCurve({std::numeric_limits<float>::infinity(),0,1,1});
  assert(editor.curve() == baseline);
  auto* input = editor.inputArea();
  input->dispatchFocusGain();
  const auto point = bezier_editor::toScreen({baseline[0],baseline[1]},{24,24,272,172});
  input->dispatchPress(point.x,point.y,BTN_LEFT,true);
  assert(editor.dragging() && editor.selectedHandle() == 0);
  input->dispatchMotion(1000,-1000);
  assert(editor.curve()[0] == 1 && editor.curve()[1] == 2 && changes == 1);
  input->dispatchCancel();
  assert(!editor.dragging() && editor.curve() == baseline && changes == 2 && ends == 1);
  input->dispatchKey(XKB_KEY_End,0,0,true);
  assert(editor.selectedHandle() == 1);
  input->dispatchKey(XKB_KEY_Right,0,0,true);
  assert(std::abs(editor.curve()[2]-0.76F) < 0.00001F && changes == 3);
  input->dispatchKey(XKB_KEY_Right,0,0,false);
  assert(ends == 2);
  input->dispatchKey(XKB_KEY_Up,0,KeyMod::Shift,true);
  assert(std::abs(editor.curve()[3]-0.6F) < 0.00001F && changes == 4);
  input->dispatchKey(XKB_KEY_Up,0,KeyMod::Shift,false);
  assert(ends == 3);
  const auto retained = editor.curve();
  const auto previous = Style::controls();
  auto controls = previous;
  controls.button_transition_ms = 0;
  Style::setControls(controls);
  editor.setScale(1.25F);
  assert(editor.curve() == retained && input->focused() && editor.selectedHandle() == 1);
  editor.setEnabled(false);
  input->dispatchKey(XKB_KEY_Left,0,0,true);
  input->dispatchPress(50,50,BTN_LEFT,true);
  assert(!editor.dragging() && editor.curve() == retained && changes == 4 && ends == 3);
  editor.setEnabled(true);
  input->dispatchKey(XKB_KEY_1,0,0,true);
  input->dispatchKey(XKB_KEY_Up,0,0,true);
  input->dispatchKey(XKB_KEY_Escape,0,0,true);
  assert(editor.curve() == retained && changes == 6 && ends == 4);
  assert(!animations.hasActive());

  // Coincident and subpixel-neighbour handles retain the keyboard-selected handle.
  BezierEditor overlap;
  overlap.setScale(1.25F);
  auto* overlapInput = overlap.inputArea();
  overlapInput->dispatchFocusGain();
  const bezier_editor::Viewport fractionalView{30,30,260,160};
  for (float separation : {0.0F, 0.0005F}) {
    const BezierEditor::Curve initial{0.3333F,0.7007F,0.3333F+separation,0.7007F};
    overlap.setCurve(initial);
    overlapInput->dispatchKey(XKB_KEY_2,0,0,true);
    const auto origin = bezier_editor::toScreen({initial[0],initial[1]},fractionalView);
    overlapInput->dispatchPress(origin.x,origin.y,BTN_LEFT,true);
    assert(overlap.dragging() && overlap.selectedHandle() == 1);
    overlapInput->dispatchMotion(origin.x+3.25F,origin.y-2.5F);
    overlapInput->dispatchPress(origin.x+3.25F,origin.y-2.5F,BTN_LEFT,false);
    assert(!overlap.dragging());
    assert(overlap.curve()[0] == initial[0] && overlap.curve()[1] == initial[1]);
    assert(overlap.curve()[2] > initial[2] && overlap.curve()[3] > initial[3]);
  }
  // Outside the tie tolerance, pointer proximity still wins over keyboard selection.
  overlap.setCurve({0.3333F,0.7007F,0.3533F,0.7007F});
  overlapInput->dispatchKey(XKB_KEY_2,0,0,true);
  const auto distinct = bezier_editor::toScreen({0.3333F,0.7007F},fractionalView);
  overlapInput->dispatchPress(distinct.x,distinct.y,BTN_LEFT,true);
  assert(overlap.dragging() && overlap.selectedHandle() == 0);
  overlapInput->dispatchCancel();
  Style::setControls(previous);
}
