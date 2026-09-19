#pragma once

#include <cstdint>
#include <functional>

class AnimationManager;
class Node;

namespace popup_transition {

enum class Phase : std::uint8_t { Idle, Opening, Open, Closing };

// Drives popup paint only. Hosts keep the final surface/input geometry mounted
// for the whole transition, so pointer and keyboard targets never move.
class Transition {
public:
  using Invalidate = std::function<void()>;

  Transition() = default;
  ~Transition();
  Transition(const Transition&) = delete;
  Transition& operator=(const Transition&) = delete;

  void open(Node& paintRoot, AnimationManager& animations, Invalidate invalidate);
  [[nodiscard]] bool close(std::function<void()> completion);
  void reset();

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  [[nodiscard]] float progress() const noexcept { return m_progress; }

private:
  void apply(float progress);

  AnimationManager* m_animations = nullptr;
  Node* m_paintRoot = nullptr;
  Invalidate m_invalidate;
  std::function<void()> m_closeCompletion;
  std::uint32_t m_animationId = 0;
  std::uint64_t m_generation = 0;
  Phase m_phase = Phase::Idle;
  float m_progress = 1.0F;
};

} // namespace popup_transition
