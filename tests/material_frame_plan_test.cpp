#include "wayland/material_frame_plan.h"

#include <cassert>

namespace {
MaterialFrameKey key(std::uint8_t payload, std::uint64_t geometry,
                     std::uint64_t target, std::uint64_t resources) {
  return MaterialFrameKey{
      .payload = {payload}, .omittedGroups = {7, 9},
      .geometryGeneration = geometry, .targetGeneration = target,
      .resourceGeneration = resources,
  };
}
}

int main() {
  using noctalia::material::kCustomEffectTransportVersion;
  using noctalia::material::kMaterialProtocolVersion;
  assert(materialEffectFramePolicy(1, true, true, true) == MaterialEffectFramePolicy::Native);
  assert(materialEffectFramePolicy(kCustomEffectTransportVersion - 1, true, true, true)
      == MaterialEffectFramePolicy::Native);
  assert(materialEffectFramePolicy(kCustomEffectTransportVersion, true, true, false)
      == MaterialEffectFramePolicy::Staged);
  assert(materialEffectFramePolicy(kCustomEffectTransportVersion, true, true, true)
      == MaterialEffectFramePolicy::Staged); // v3 has no exact-frame arm; native stays visible.
  assert(materialEffectFramePolicy(kExactMaterialFrameArmVersion, true, true, false)
      == MaterialEffectFramePolicy::Staged);
  assert(materialEffectFramePolicy(kExactMaterialFrameArmVersion, true, true, true)
      == MaterialEffectFramePolicy::Armed);
  assert(materialEffectFramePolicy(kMaterialProtocolVersion, true, true, true)
      == MaterialEffectFramePolicy::Armed);
  assert(materialEffectFramePolicy(kExactMaterialFrameArmVersion, false, true, true)
      == MaterialEffectFramePolicy::Native);
  assert(materialEffectFramePolicy(kExactMaterialFrameArmVersion, true, false, true)
      == MaterialEffectFramePolicy::Native);

  MaterialFramePlanState state;
  const auto first = key(1, 10, 20, 30);
  state.requested(4, first);
  assert(state.waitingForArm());
  assert(!state.armed(3));
  assert(!state.prepare(first));
  assert(state.armed(4));
  const auto prepared = state.prepare(first);
  assert(prepared && prepared->serial == 4 && prepared->key.omittedGroups.size() == 2);

  assert(!state.prepare(key(2, 10, 20, 30))); // payload changed
  assert(!state.prepare(key(1, 11, 20, 30))); // geometry changed
  assert(!state.prepare(key(1, 10, 21, 30))); // target changed
  assert(!state.prepare(key(1, 10, 20, 31))); // imported resource generation changed
  assert(!state.committed(PreparedMaterialFrame{5, first}));
  assert(state.committed(*prepared));
  assert(!state.applied(3));
  assert(state.applied(4));

  state.requested(8, first);
  state.invalidate(); // disconnect, reject, context loss, or route rebuild
  assert(!state.armed(8));
  assert(!state.prepare(first));

  state.requested(9, first);
  assert(state.armed(9));
  state.requested(10, key(3, 10, 20, 30)); // a newer descriptor revokes the old arm
  assert(!state.prepare(first));
  assert(!state.armed(9));
  assert(state.armed(10));

  // A receiver/plugin failure after arm revokes omission. The next frame uses
  // native pixels because readiness is false, and cannot reuse the old arm.
  const auto armedAfterFailure = state.prepare(key(3, 10, 20, 30));
  assert(armedAfterFailure);
  state.invalidate();
  assert(materialEffectFramePolicy(kExactMaterialFrameArmVersion, true, false, true)
      == MaterialEffectFramePolicy::Native);
  assert(!state.prepare(key(3, 10, 20, 30)));

  // Reconnect may make the asset ready again, but it must pass through a new
  // staged acknowledgement before any exact-frame arm is eligible.
  assert(materialEffectFramePolicy(kExactMaterialFrameArmVersion, true, true, false)
      == MaterialEffectFramePolicy::Staged);
  return 0;
}
