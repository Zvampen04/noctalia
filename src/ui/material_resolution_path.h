#pragma once

#include "ui/material_overrides.h"

#include <span>
#include <string_view>

namespace Style {

// Resolves one semantic element through its ordered surface ancestry. The ordinary
// role/family defaults are applied once, followed by broad-to-specific surfaces such
// as bar -> bar instance -> named section -> widget instance.
inline noctalia::material::Parameters resolveMaterialPath(
    noctalia::material::Parameters base, const MaterialOverrides& overrides,
    std::string_view role, std::string_view family, std::span<const std::string_view> surfaces) noexcept {
  const auto apply = [&base](const MaterialOverrideMap& scope, std::string_view id) {
    if (const auto entry = scope.find(id); entry != scope.end()) entry->second.apply(base);
  };
  apply(overrides.roles, role);
  apply(overrides.families, family);
  for (const auto surface : surfaces) apply(overrides.surfaces, surface);
  return noctalia::material::sanitize(base);
}

inline std::optional<ResolvedCustomEffect> resolveCustomEffectPath(
    const MaterialOverrides& overrides, std::string_view role, std::string_view family,
    std::span<const std::string_view> surfaces) noexcept {
  ResolvedCustomEffect result;
  const auto apply = [&result](const MaterialOverrideMap& scope, std::string_view id) {
    if (const auto entry = scope.find(id); entry != scope.end())
      applyCustomEffectOverride(result, entry->second);
  };
  apply(overrides.roles, role);
  apply(overrides.families, family);
  for (const auto surface : surfaces) apply(overrides.surfaces, surface);
  if (result.stableId.empty() || result.sha256Digest.empty()) return std::nullopt;
  return result;
}

} // namespace Style
