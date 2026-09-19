#pragma once

#include "ui/material_overrides.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace noctalia::bar {

inline constexpr std::string_view kBarWidgetPlacementTokenPrefix = "@widget:";

namespace detail {
  inline std::uint64_t stableMaterialHash(std::string_view value) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    return hash;
  }

  inline std::string hashSuffix(std::uint64_t value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = result.size(); index-- > 0;) {
      result[index] = digits[value & 0x0fU];
      value >>= 4U;
    }
    return result;
  }
}

inline std::string stableMaterialTarget(std::string_view prefix, std::string_view identity) {
  std::string readable;
  readable.reserve(prefix.size() + identity.size() + 1);
  readable.append(prefix);
  readable.push_back('.');
  readable.append(identity);
  if (Style::validMaterialTarget(readable)) return readable;
  return std::string(prefix) + ".id-" + detail::hashSuffix(detail::stableMaterialHash(identity));
}

inline std::string barMaterialTarget(std::string_view barName) {
  return stableMaterialTarget("bar.instance", barName);
}

inline std::string barSectionMaterialTarget(std::string_view barName, std::string_view sectionId) {
  std::string identity(barName);
  identity.push_back('.');
  identity.append(sectionId);
  return stableMaterialTarget("bar.section", identity);
}

// The caller supplies the placement's persistent ID, not its widget config name or lane index.
// Reorder keeps this ID; duplicate creates a new ID even when both placements use the same type/config.
inline std::string barWidgetMaterialTarget(std::string_view widgetPlacementId) {
  return stableMaterialTarget("bar.widget", widgetPlacementId);
}

inline bool isBarWidgetPlacementToken(std::string_view value) noexcept {
  return value.starts_with(kBarWidgetPlacementTokenPrefix)
      && value.size() > kBarWidgetPlacementTokenPrefix.size();
}

inline std::string makeBarWidgetPlacementToken(std::string_view placementId) {
  return std::string(kBarWidgetPlacementTokenPrefix) + std::string(placementId);
}

inline std::string_view barWidgetPlacementTokenId(std::string_view token) noexcept {
  return isBarWidgetPlacementToken(token) ? token.substr(kBarWidgetPlacementTokenPrefix.size()) : std::string_view{};
}

struct ResolvedBarWidgetLaneEntry {
  std::string_view widgetConfigName;
  std::string_view placementId;
  [[nodiscard]] bool isPlacement() const noexcept { return !placementId.empty(); }
};

// Token-looking text is a placement only when a matching record exists. This preserves a
// pre-existing legal widget config literally named "@widget:..." and keeps storage tokens out
// of runtime registry/service lookups and user-facing summaries.
inline ResolvedBarWidgetLaneEntry resolveBarWidgetLaneEntry(
    std::string_view entry, const std::unordered_map<std::string, std::string>& placements) noexcept {
  const auto id = barWidgetPlacementTokenId(entry);
  if (!id.empty()) {
    if (const auto found = placements.find(std::string(id)); found != placements.end())
      return {.widgetConfigName = found->second, .placementId = found->first};
  }
  return {.widgetConfigName = entry};
}

// Used only when converting a legacy string lane to persistent placement records. The caller
// saves the returned ID immediately; normal reorder and duplication never regenerate it.
inline std::string legacyBarWidgetPlacementId(
    std::string_view barName, std::string_view sectionId, std::string_view widgetName,
    std::size_t legacyOccurrence, const std::unordered_set<std::string>& occupied) {
  std::string seed;
  seed.reserve(barName.size() + sectionId.size() + widgetName.size() + 32);
  seed.append(barName).push_back('\0');
  seed.append(sectionId).push_back('\0');
  seed.append(widgetName).push_back('\0');
  seed.append(std::to_string(legacyOccurrence));
  const std::string base = "legacy-" + detail::hashSuffix(detail::stableMaterialHash(seed));
  if (!occupied.contains(base)) return base;
  for (std::size_t suffix = 2;; ++suffix) {
    const std::string candidate = base + '-' + std::to_string(suffix);
    if (!occupied.contains(candidate)) return candidate;
  }
}

} // namespace noctalia::bar
