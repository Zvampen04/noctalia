#pragma once

#include "material/scene_transport.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

struct MaterialFrameKey {
  std::vector<std::uint8_t> payload;
  std::vector<std::uint32_t> omittedGroups;
  std::uint64_t geometryGeneration = 0;
  std::uint64_t targetGeneration = 0;
  std::uint64_t resourceGeneration = 0;
  bool operator==(const MaterialFrameKey&) const = default;
};

struct PreparedMaterialFrame {
  std::uint32_t serial = 0;
  MaterialFrameKey key;
  bool operator==(const PreparedMaterialFrame&) const = default;
};

enum class MaterialEffectFramePolicy {
  Native,
  Staged,
  Armed,
};

inline constexpr std::uint32_t kExactMaterialFrameArmVersion = 4;

// Protocol v3 can import and validate effects, but it has no exact-frame arm.
// It therefore keeps custom descriptors staged and the native body visible.
// v4 and newer may progress from an applied staged descriptor to an exact
// armed frame. Protocol v5 routes continuous ownership through the separate
// lease state machine after preserving this compatibility path.
[[nodiscard]] constexpr MaterialEffectFramePolicy materialEffectFramePolicy(
    std::uint32_t protocolVersion, bool hasEffects, bool allEffectsReady, bool stagedApplied) noexcept {
  if (!hasEffects || !allEffectsReady
      || protocolVersion < noctalia::material::kCustomEffectTransportVersion)
    return MaterialEffectFramePolicy::Native;
  if (protocolVersion < kExactMaterialFrameArmVersion || !stagedApplied)
    return MaterialEffectFramePolicy::Staged;
  return MaterialEffectFramePolicy::Armed;
}

// Pure state machine for the v4 active-scene handshake. A compositor `armed`
// event authorizes exactly one immutable descriptor/generation tuple. Callers
// still retain native rendering for protocol v1-v3 and for every mismatch.
class MaterialFramePlanState {
public:
  void requested(std::uint32_t serial, MaterialFrameKey key) {
    m_requested = PreparedMaterialFrame{serial, std::move(key)};
    m_armed.reset();
  }

  [[nodiscard]] bool armed(std::uint32_t serial) {
    if (!m_requested || m_requested->serial != serial) return false;
    m_armed = std::move(m_requested);
    m_requested.reset();
    return true;
  }

  [[nodiscard]] std::optional<PreparedMaterialFrame> prepare(const MaterialFrameKey& current) const {
    if (!m_armed || m_armed->key != current) return std::nullopt;
    return m_armed;
  }

  [[nodiscard]] bool committed(const PreparedMaterialFrame& frame) {
    if (!m_armed || *m_armed != frame) return false;
    m_committedSerial = frame.serial;
    m_armed.reset();
    return true;
  }

  [[nodiscard]] bool applied(std::uint32_t serial) {
    if (m_committedSerial != serial) return false;
    m_committedSerial = 0;
    return true;
  }

  void invalidate() {
    m_requested.reset();
    m_armed.reset();
    m_committedSerial = 0;
  }

  [[nodiscard]] bool waitingForArm() const noexcept { return m_requested.has_value(); }
  [[nodiscard]] bool requestedMatches(const MaterialFrameKey& key) const {
    return m_requested && m_requested->key == key;
  }
  [[nodiscard]] bool hasArmedFrame() const noexcept { return m_armed.has_value(); }

private:
  std::optional<PreparedMaterialFrame> m_requested;
  std::optional<PreparedMaterialFrame> m_armed;
  std::uint32_t m_committedSerial = 0;
};
