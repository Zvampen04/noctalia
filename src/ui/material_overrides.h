#pragma once

#include "material/material.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace Style {

inline constexpr std::size_t kCustomEffectParameterCount = 32;
inline constexpr float kCustomEffectParameterMin = -16.0F;
inline constexpr float kCustomEffectParameterMax = 16.0F;
inline constexpr float kCustomEffectSampleRadiusMax = 256.0F;

struct MaterialOverride {
  std::optional<noctalia::material::Primitive> primitive;
  std::optional<std::string> customBackground;
  std::optional<std::string> customBackgroundDigest;
  std::optional<float> customSampleRadiusPx;
  std::array<std::optional<float>, kCustomEffectParameterCount> customParameters;
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) std::optional<float> key;
#include "material/fields.def"
#undef MATERIAL_FIELD
  bool operator==(const MaterialOverride&) const = default;

  void apply(noctalia::material::Parameters& result) const noexcept {
    if (primitive) result.primitive = *primitive;
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) if (key) result.member = *key;
#include "material/fields.def"
#undef MATERIAL_FIELD
  }
};

struct ResolvedCustomEffect {
  std::string stableId;
  std::string sha256Digest;
  float maxSampleRadiusPx = 0.0F;
  std::array<float, kCustomEffectParameterCount> parameters{};
  bool operator==(const ResolvedCustomEffect&) const = default;
};

using MaterialOverrideMap = std::map<std::string, MaterialOverride, std::less<>>;
struct MaterialOverrides {
  MaterialOverrideMap roles;
  MaterialOverrideMap families;
  MaterialOverrideMap surfaces;
  bool operator==(const MaterialOverrides&) const = default;
};

struct MaterialTarget {
  std::string_view id;
  std::string_view label;
  std::string_view role;
  std::string_view family;
};
inline constexpr std::array kMaterialRoles{
    MaterialTarget{"surface", "Surface", "", ""},
    MaterialTarget{"control", "Control", "", ""},
    MaterialTarget{"raised", "Raised surface", "", ""},
    MaterialTarget{"inset", "Inset surface", "", ""},
    MaterialTarget{"overlay", "Overlay", "", ""},
};
inline constexpr std::array kMaterialFamilies{
    MaterialTarget{"button", "Buttons", "control", ""},
    MaterialTarget{"input", "Text inputs", "control", ""},
    MaterialTarget{"select", "Selectors", "control", ""},
    MaterialTarget{"toggle", "Toggles", "control", ""},
    MaterialTarget{"toggle-well", "Switch recessed track", "control", ""},
    MaterialTarget{"settings-row", "Connected settings row", "surface", "settings"},
    MaterialTarget{"segmented-indicator", "Segmented moving indicator", "control", ""},
    MaterialTarget{"toggle-indicator", "Switch moving indicator", "control", ""},
    MaterialTarget{"checkbox-well", "Checkbox recessed well", "control", ""},
    MaterialTarget{"checkbox-plateau", "Checkbox central plateau", "control", ""},
    MaterialTarget{"card", "Raised cards", "surface", ""},
    MaterialTarget{"slider", "Sliders", "control", ""},
    MaterialTarget{"scrollbar", "Scrollbars", "control", ""},
    MaterialTarget{"progress", "Progress indicators", "control", ""},
    MaterialTarget{"stepper", "Steppers", "control", ""},
    MaterialTarget{"segmented", "Segmented controls", "control", ""},
    MaterialTarget{"container", "Containers", "surface", ""},
    MaterialTarget{"panel", "Panels", "surface", ""},
    MaterialTarget{"bar", "Bars", "surface", ""},
    MaterialTarget{"bar-widget", "Bar buttons", "surface", "bar"},
    MaterialTarget{"launcher-row", "Launcher results", "surface", "panel"},
    MaterialTarget{"dock", "Docks", "surface", ""},
    MaterialTarget{"notification", "Notifications", "surface", ""},
    MaterialTarget{"osd", "On-screen indicators", "surface", ""},
};
inline constexpr std::array kMaterialSurfaces{
    MaterialTarget{"bar", "Bar surface", "surface", "bar"},
    MaterialTarget{"panel", "Panel surface", "surface", "panel"},
    MaterialTarget{"dock", "Dock surface", "surface", "dock"},
    MaterialTarget{"notification", "Notification surface", "surface", "notification"},
    MaterialTarget{"osd", "On-screen indicator surface", "surface", "osd"},
    MaterialTarget{"settings", "Settings window", "surface", "container"},
    MaterialTarget{"window_switcher", "Window switcher", "surface", "container"},
    MaterialTarget{"tooltip", "Tooltips", "surface", "container"},
    MaterialTarget{"tray", "Tray surfaces", "surface", "container"},
    MaterialTarget{"system-dialogs", "System dialogs", "surface", "panel"},
    MaterialTarget{"desktop", "Desktop widgets", "surface", "container"},
    MaterialTarget{"window.frame", "Window frame", "surface", "container"},
    MaterialTarget{"window.background", "Transparent application backgrounds", "surface", "container"},
    MaterialTarget{"lock", "Lock screen", "surface", "container"},
    MaterialTarget{"greeter", "Login screen", "surface", "container"},
    MaterialTarget{"osk", "On-screen keyboard", "surface", "container"},
};

inline bool validMaterialTarget(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 96
      && std::ranges::all_of(value, [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_' || c == '.';
         });
}
inline MaterialOverrideMap* materialScope(MaterialOverrides& overrides, std::string_view scope) noexcept {
  if (scope == "roles") return &overrides.roles;
  if (scope == "families") return &overrides.families;
  if (scope == "surfaces") return &overrides.surfaces;
  return nullptr;
}
inline const MaterialOverrideMap* materialScope(const MaterialOverrides& overrides, std::string_view scope) noexcept {
  if (scope == "roles") return &overrides.roles;
  if (scope == "families") return &overrides.families;
  if (scope == "surfaces") return &overrides.surfaces;
  return nullptr;
}

inline void applyCustomEffectOverride(ResolvedCustomEffect& result, const MaterialOverride& patch) noexcept {
  if (patch.customBackground) result.stableId = *patch.customBackground;
  if (patch.customBackgroundDigest) result.sha256Digest = *patch.customBackgroundDigest;
  if (patch.customSampleRadiusPx)
    result.maxSampleRadiusPx = std::clamp(*patch.customSampleRadiusPx, 0.0F, kCustomEffectSampleRadiusMax);
  for (std::size_t index = 0; index < patch.customParameters.size(); ++index)
    if (patch.customParameters[index])
      result.parameters[index] = std::clamp(
          *patch.customParameters[index], kCustomEffectParameterMin, kCustomEffectParameterMax);
}

inline std::optional<ResolvedCustomEffect> resolveCustomEffect(
    const MaterialOverrides& overrides, std::string_view role = {},
    std::string_view family = {}, std::string_view surface = {}) noexcept {
  ResolvedCustomEffect result;
  const auto apply = [&result](const MaterialOverrideMap& scope, std::string_view id) {
    if (const auto entry = scope.find(id); entry != scope.end())
      applyCustomEffectOverride(result, entry->second);
  };
  apply(overrides.roles, role);
  apply(overrides.families, family);
  apply(overrides.surfaces, surface);
  if (result.stableId.empty() || result.sha256Digest.empty()) return std::nullopt;
  return result;
}
inline std::optional<noctalia::material::Primitive> materialPrimitive(std::string_view value) noexcept {
  using Primitive = noctalia::material::Primitive;
  if (value == "flat") return Primitive::Flat;
  if (value == "neumorphic") return Primitive::Plateau;
  if (value == "liquid_glass") return Primitive::Optical;
  if (value == "illustrated") return Primitive::Illustrated;
  return std::nullopt;
}
inline std::string_view materialPrimitiveName(noctalia::material::Primitive value) noexcept {
  using Primitive = noctalia::material::Primitive;
  switch (value) {
  case Primitive::Flat: return "flat";
  case Primitive::Plateau: return "neumorphic";
  case Primitive::Optical: return "liquid_glass";
  case Primitive::Illustrated: return "illustrated";
  }
  return "flat";
}

inline noctalia::material::Parameters resolveMaterial(
    noctalia::material::Parameters base, const MaterialOverrides& overrides,
    std::string_view role = {}, std::string_view family = {}, std::string_view surface = {}) noexcept {
  const auto apply = [&base](const MaterialOverrideMap& scope, std::string_view id) {
    const auto entry = scope.find(id);
    if (entry != scope.end()) entry->second.apply(base);
  };
  apply(overrides.roles, role);
  apply(overrides.families, family);
  apply(overrides.surfaces, surface);
  return noctalia::material::sanitize(base);
}

} // namespace Style
