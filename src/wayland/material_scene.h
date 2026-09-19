#pragma once
#include "material/scene_descriptor.h"
#include "wayland/material_frame_plan.h"
#include "wayland/material_lease_plan.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>
class Node;
class CustomEffectTransportRegistry;
struct CustomEffectAsset;
struct wl_surface;
struct noctalia_material_manager_v1;
struct noctalia_material_surface_v1;

// Retained semantic surfaces become grouped optical planes. All foreground
// content remains in the ordinary client buffer, above the compositor lens.
noctalia::material::SceneDescriptor collectMaterialScene(const Node* root, float width, float height);
// Protocol-v5 lease collection retains live custom-effect planes at zero
// opacity so popup entrances can prewarm one owner before the first paint.
noctalia::material::SceneDescriptor collectMaterialLeaseScene(const Node* root, float width, float height);
class MaterialSceneSender {
public:
  enum class FrameMode { Native, Defer, Omit };
  struct FramePlan {
    FrameMode mode = FrameMode::Native;
    std::vector<std::uint32_t> omittedGroups;
    std::optional<PreparedMaterialFrame> prepared;
    std::optional<PreparedMaterialLeaseFrame> lease;
    std::vector<std::uint8_t> precommitPayload;
    bool sceneManaged = false;
    [[nodiscard]] bool needsCommit() const noexcept {
      return prepared.has_value() || lease.has_value();
    }
  };

  ~MaterialSceneSender();
  [[nodiscard]] FramePlan prepare(
      noctalia_material_manager_v1* manager, CustomEffectTransportRegistry* effects,
      wl_surface* surface, const Node* root, float width, float height,
      std::function<void()> requestRedraw);
  [[nodiscard]] bool commit(const FramePlan& plan);
  void send(noctalia_material_manager_v1* manager, CustomEffectTransportRegistry* effects,
            wl_surface* surface, const Node* root, float width, float height,
            std::function<void()> requestRedraw);
  void reset();
private:
  void handleApplied(std::uint32_t serial);
  void handleRejected(std::uint32_t serial);
  void handleArmed(std::uint32_t serial);
  void handleLeaseGranted(std::uint32_t requestSerial, std::uint32_t token);
  void handleLeaseRejected(std::uint32_t requestSerial);
  void handleLeaseArmed(std::uint32_t sceneSerial, std::uint32_t token);
  void handleLeaseRevoked(std::uint32_t token);
  void handleLeaseReleased(std::uint32_t token, std::uint32_t sceneSerial);
  void reconnectEffects(CustomEffectTransportRegistry* effects, std::function<void()> requestRedraw);
  void syncRetainedEffects(std::span<const std::shared_ptr<const CustomEffectAsset>> assets);
  void releaseRetainedEffects();
  [[nodiscard]] std::uint32_t sendPayload(
      noctalia_material_manager_v1* manager, wl_surface* surface,
      std::span<const std::uint8_t> payload);

  noctalia_material_surface_v1* m_resource = nullptr;
  CustomEffectTransportRegistry* m_effects = nullptr;
  std::function<void()> m_requestRedraw;
  std::vector<std::uint8_t> m_lastPayload;
  std::vector<std::uint8_t> m_pendingStagedActivePayload;
  std::vector<std::uint8_t> m_appliedStagedActivePayload;
  std::vector<std::uint8_t> m_pendingStagedWirePayload;
  std::vector<std::uint8_t> m_appliedStagedWirePayload;
  std::vector<noctalia::material::CustomEffectTransportDigest> m_effectSignature;
  std::vector<noctalia::material::CustomEffectTransportDigest> m_retainedEffects;
  std::uint64_t m_effectManagerGeneration = 0;
  std::uint32_t m_serial = 0;
  std::uint32_t m_pendingStagedSerial = 0;
  std::uint32_t m_appliedStagedSerial = 0;
  std::uint32_t m_pendingActiveSerial = 0;
  std::uint32_t m_leaseRequestSerial = 0;
  std::uint64_t m_effectSubscription = 0;
  bool m_stagedApplied = false;
  bool m_activeApplied = false; // Informational only; native body is retained in this slice.
  bool m_supportsArmedFrames = false;
  bool m_supportsLeases = false;
  bool m_leaseRejected = false;
  MaterialFramePlanState m_framePlans;
  MaterialLeasePlanState m_leasePlans;
  std::vector<std::uint8_t> m_planPayload;
  std::uint64_t m_geometryGeneration = 1;
  std::uint64_t m_targetGeneration = 1;
  noctalia_material_manager_v1* m_manager = nullptr;
  wl_surface* m_surface = nullptr;
};
