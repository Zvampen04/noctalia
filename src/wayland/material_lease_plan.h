#pragma once

#include "material/custom_effect_lease.h"

#include <cstdint>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

struct MaterialLeaseFrameKey {
  std::vector<std::uint8_t> payload;
  std::vector<std::uint32_t> omittedGroups;
  noctalia::material::CustomEffectLeaseSignature signature;
  std::uint64_t targetGeneration = 0;
  std::uint64_t resourceGeneration = 0;
  bool operator==(const MaterialLeaseFrameKey&) const = default;
};

struct PreparedMaterialLeaseFrame {
  enum class Kind { First, Streaming, Restore };
  Kind kind = Kind::First;
  std::uint32_t token = 0;
  std::uint64_t revision = 0;
  MaterialLeaseFrameKey key;
  bool operator==(const PreparedMaterialLeaseFrame&) const = default;
};

// Pure sender-side state for protocol-v5 continuous ownership. A granted token
// authorizes one ordered target/resource signature. Geometry and bounded
// presentation fields may change while Active, but target or resource changes
// must first restore a token-zero scene with native pixels in the same commit.
class MaterialLeasePlanState {
public:
  enum class Phase { Idle, Requested, Granted, InitialRequested, InitialArmed, Active, Restoring };

  bool requested(std::uint32_t requestSerial, std::uint32_t stagedSceneSerial,
                 noctalia::material::CustomEffectLeaseSignature signature) {
    if (m_phase != Phase::Idle || requestSerial == 0 || stagedSceneSerial == 0 || signature.empty()) return false;
    m_requestSerial = requestSerial;
    m_stagedSceneSerial = stagedSceneSerial;
    m_signature = std::move(signature);
    m_phase = Phase::Requested;
    ++m_revision;
    return true;
  }

  bool granted(std::uint32_t requestSerial, std::uint32_t token) {
    if (m_phase != Phase::Requested || requestSerial != m_requestSerial || token == 0
        || m_seenTokens.contains(token)) return false;
    m_seenTokens.insert(token);
    m_token = token;
    m_phase = Phase::Granted;
    ++m_revision;
    return true;
  }

  bool rejected(std::uint32_t requestSerial) {
    if (m_phase != Phase::Requested || requestSerial != m_requestSerial) return false;
    clearCurrent();
    ++m_revision;
    return true;
  }

  bool initialRequested(std::uint32_t sceneSerial, MaterialLeaseFrameKey key) {
    if ((m_phase != Phase::Granted && m_phase != Phase::InitialRequested
         && m_phase != Phase::InitialArmed)
        || sceneSerial == 0 || !compatible(key)) return false;
    m_initialSceneSerial = sceneSerial;
    m_initialKey = std::move(key);
    m_phase = Phase::InitialRequested;
    ++m_revision;
    return true;
  }

  bool armed(std::uint32_t sceneSerial, std::uint32_t token) {
    if (m_phase != Phase::InitialRequested || sceneSerial != m_initialSceneSerial || token != m_token) return false;
    m_phase = Phase::InitialArmed;
    ++m_revision;
    return true;
  }

  std::optional<PreparedMaterialLeaseFrame> prepare(const MaterialLeaseFrameKey& current) const {
    if (current.signature != m_signature || current.targetGeneration != m_targetGeneration
        || current.resourceGeneration != m_resourceGeneration) return std::nullopt;
    if (m_phase == Phase::InitialArmed) {
      if (!m_initialKey || *m_initialKey != current) return std::nullopt;
      return PreparedMaterialLeaseFrame{PreparedMaterialLeaseFrame::Kind::First, m_token, m_revision, current};
    }
    if (m_phase == Phase::Active)
      return PreparedMaterialLeaseFrame{PreparedMaterialLeaseFrame::Kind::Streaming, m_token, m_revision, current};
    return std::nullopt;
  }

  bool committed(const PreparedMaterialLeaseFrame& frame) {
    if (!accepts(frame)) return false;
    if (frame.kind == PreparedMaterialLeaseFrame::Kind::First && m_phase == Phase::InitialArmed) {
      m_phase = Phase::Active;
      m_initialKey.reset();
      ++m_revision;
      return true;
    }
    return frame.kind == PreparedMaterialLeaseFrame::Kind::Streaming && m_phase == Phase::Active;
  }

  [[nodiscard]] bool accepts(const PreparedMaterialLeaseFrame& frame) const {
    if (frame.revision != m_revision || frame.token != m_token) return false;
    if (frame.kind == PreparedMaterialLeaseFrame::Kind::Restore)
      return m_phase == Phase::Restoring && m_restoreSceneSerial == 0;
    if (frame.key.signature != m_signature) return false;
    if (frame.kind == PreparedMaterialLeaseFrame::Kind::First) return m_phase == Phase::InitialArmed;
    if (frame.kind == PreparedMaterialLeaseFrame::Kind::Streaming) return m_phase == Phase::Active;
    return false;
  }

  bool beginRestore() {
    if (m_token == 0 || m_phase == Phase::Idle || m_phase == Phase::Restoring) return false;
    m_phase = Phase::Restoring;
    m_initialKey.reset();
    ++m_revision;
    return true;
  }

  void invalidate() {
    if (m_token != 0) {
      (void)beginRestore();
      return;
    }
    clearCurrent();
    ++m_revision;
  }

  std::optional<PreparedMaterialLeaseFrame> prepareRestore(MaterialLeaseFrameKey key) const {
    if (m_phase != Phase::Restoring || m_token == 0 || m_restoreSceneSerial != 0) return std::nullopt;
    return PreparedMaterialLeaseFrame{PreparedMaterialLeaseFrame::Kind::Restore, m_token, m_revision, std::move(key)};
  }

  bool restoreCommitted(const PreparedMaterialLeaseFrame& frame, std::uint32_t sceneSerial) {
    if (m_phase != Phase::Restoring || frame.kind != PreparedMaterialLeaseFrame::Kind::Restore
        || frame.revision != m_revision || frame.token != m_token || sceneSerial == 0) return false;
    m_restoreSceneSerial = sceneSerial;
    return true;
  }

  bool revoked(std::uint32_t token) {
    if (token == 0 || token != m_token) return false;
    return beginRestore() || m_phase == Phase::Restoring;
  }

  bool released(std::uint32_t token, std::uint32_t sceneSerial) {
    if (m_phase != Phase::Restoring || token != m_token || sceneSerial != m_restoreSceneSerial) return false;
    clearCurrent();
    ++m_revision;
    return true;
  }

  void setGenerations(std::uint64_t target, std::uint64_t resources) {
    if (m_phase != Phase::Idle) return;
    m_targetGeneration = target;
    m_resourceGeneration = resources;
  }

  void resetBinding() {
    clearCurrent();
    m_seenTokens.clear();
    ++m_revision;
  }

  [[nodiscard]] Phase phase() const noexcept { return m_phase; }
  [[nodiscard]] std::uint32_t token() const noexcept { return m_token; }
  [[nodiscard]] std::uint32_t requestSerial() const noexcept { return m_requestSerial; }
  [[nodiscard]] const auto& signature() const noexcept { return m_signature; }
  [[nodiscard]] bool ownsBody() const noexcept {
    return m_phase == Phase::InitialArmed || m_phase == Phase::Active;
  }
  [[nodiscard]] bool initialRequestedMatches(const MaterialLeaseFrameKey& key) const {
    return m_phase == Phase::InitialRequested && m_initialKey && *m_initialKey == key;
  }
  [[nodiscard]] bool compatible(const MaterialLeaseFrameKey& key) const {
    return key.signature == m_signature && key.targetGeneration == m_targetGeneration
        && key.resourceGeneration == m_resourceGeneration;
  }
  [[nodiscard]] bool compatible(const noctalia::material::CustomEffectLeaseSignature& signature,
                                std::uint64_t target, std::uint64_t resources) const {
    return signature == m_signature && target == m_targetGeneration && resources == m_resourceGeneration;
  }

private:
  void clearCurrent() {
    m_phase = Phase::Idle;
    m_requestSerial = 0;
    m_stagedSceneSerial = 0;
    m_initialSceneSerial = 0;
    m_restoreSceneSerial = 0;
    m_token = 0;
    m_signature.clear();
    m_initialKey.reset();
  }

  Phase m_phase = Phase::Idle;
  std::uint32_t m_requestSerial = 0;
  std::uint32_t m_stagedSceneSerial = 0;
  std::uint32_t m_initialSceneSerial = 0;
  std::uint32_t m_restoreSceneSerial = 0;
  std::uint32_t m_token = 0;
  std::uint64_t m_revision = 1;
  std::uint64_t m_targetGeneration = 0;
  std::uint64_t m_resourceGeneration = 0;
  noctalia::material::CustomEffectLeaseSignature m_signature;
  std::optional<MaterialLeaseFrameKey> m_initialKey;
  std::unordered_set<std::uint32_t> m_seenTokens;
};
