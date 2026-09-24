#pragma once

#include <span>
#include <string>
#include <string_view>

namespace attached_panel {

// A page-specific route takes precedence over the panel's default route.
// Explicit click sources are resolved by the caller before consulting these.
inline std::string sourceSection(std::span<const std::string> routes,
                                 std::string_view panel, std::string_view context) {
  const auto fallback = std::string(panel) + "=";
  const auto specific = std::string(panel) + "/" + std::string(context) + "=";
  std::string section;
  for (const auto& route : routes) {
    if (!context.empty() && route.starts_with(specific)) return route.substr(specific.size());
    if (section.empty() && route.starts_with(fallback)) section = route.substr(fallback.size());
  }
  return section;
}

} // namespace attached_panel
