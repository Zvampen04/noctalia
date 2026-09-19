#include "ui/popup_transition.h"

#include "render/animation/animation.h"
#include "render/animation/animation_manager.h"
#include "render/scene/node.h"
#include "ui/style.h"

#include <algorithm>
#include <utility>

namespace popup_transition {

Transition::~Transition() { reset(); }

void Transition::apply(float progress) {
  m_progress = std::clamp(progress, 0.0F, 1.0F);
  if (m_paintRoot != nullptr) {
    // Opacity leaves the final layout and input transforms unchanged. The
    // fixed clipping node contains shadow, material background and content,
    // so native paint and exported material planes share the same fade.
    m_paintRoot->setOpacity(m_progress);
  }
  if (m_invalidate) m_invalidate();
}

void Transition::open(Node& paintRoot, AnimationManager& animations, Invalidate invalidate) {
  reset();
  m_animations = &animations;
  m_paintRoot = &paintRoot;
  m_invalidate = std::move(invalidate);
  m_phase = Phase::Opening;
  const auto generation = ++m_generation;
  apply(0.0F);
  m_animationId = animations.animate(
      0.0F, 1.0F, Style::animFast, Easing::EaseOutCubic,
      [this, generation](float progress) {
        if (generation == m_generation) apply(progress);
      },
      [this, generation]() {
        if (generation != m_generation) return;
        m_animationId = 0;
        m_phase = Phase::Open;
        apply(1.0F);
      },
      this
  );
}

bool Transition::close(std::function<void()> completion) {
  if (m_animations == nullptr || m_paintRoot == nullptr || m_phase == Phase::Idle || m_phase == Phase::Closing) {
    return false;
  }
  if (m_animationId != 0) m_animations->cancel(m_animationId);
  m_closeCompletion = std::move(completion);
  m_phase = Phase::Closing;
  const auto generation = ++m_generation;
  m_animationId = m_animations->animate(
      m_progress, 0.0F, Style::animFast, Easing::EaseOutCubic,
      [this, generation](float progress) {
        if (generation == m_generation) apply(progress);
      },
      [this, generation]() {
        if (generation != m_generation) return;
        m_animationId = 0;
        m_phase = Phase::Idle;
        apply(0.0F);
        auto completion = std::move(m_closeCompletion);
        if (completion) completion();
      },
      this
  );
  return true;
}

void Transition::reset() {
  ++m_generation;
  if (m_animations != nullptr && m_animationId != 0) m_animations->cancel(m_animationId);
  m_animationId = 0;
  m_closeCompletion = {};
  if (m_paintRoot != nullptr) m_paintRoot->setOpacity(1.0F);
  m_animations = nullptr;
  m_paintRoot = nullptr;
  m_invalidate = {};
  m_progress = 1.0F;
  m_phase = Phase::Idle;
}

} // namespace popup_transition
