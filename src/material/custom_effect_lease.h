#pragma once

#include "scene_descriptor.h"

#include <cstdint>
#include <string>
#include <vector>

namespace noctalia::material {
struct CustomEffectLeaseTarget {
  std::uint32_t group = 0;
  std::string role;
  std::string surface;
  CustomEffectTransportDigest transportDigest{};
  std::uint32_t abi = 0;
  bool operator==(const CustomEffectLeaseTarget&) const = default;
};
using CustomEffectLeaseSignature = std::vector<CustomEffectLeaseTarget>;

// The signature fixes target identity and compiled resources while leaving all
// bounded presentation fields (geometry, opacity, tint, radius and parameters)
// free to change in codec-v7 frames.
[[nodiscard]] inline CustomEffectLeaseSignature customEffectLeaseSignature(const SceneDescriptor& scene) {
  CustomEffectLeaseSignature result;
  for (const auto& plane : scene.planes) {
    if (!plane.customEffect) continue;
    result.push_back(CustomEffectLeaseTarget{
        .group = plane.group,
        .role = plane.role,
        .surface = plane.surface,
        .transportDigest = plane.customEffect->transportDigest,
        .abi = plane.customEffect->abi,
    });
  }
  return result;
}

[[nodiscard]] inline bool validCustomEffectLeaseScene(
    const SceneDescriptor& scene, std::uint32_t token,
    const CustomEffectLeaseSignature& signature) {
  return token != 0 && scene.leaseToken == token && validateScene(scene)
      && !signature.empty() && customEffectLeaseSignature(scene) == signature;
}
}
