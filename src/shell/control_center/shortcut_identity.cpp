#include "shell/control_center/shortcut_identity.h"

#include "ui/material_overrides.h"

#include <iomanip>
#include <set>
#include <sstream>

namespace control_center_material {
namespace {
std::string legacyToken(std::string_view type, std::size_t ordinal) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : type) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << "legacy-" << std::hex << std::setw(16) << std::setfill('0') << hash << '-' << ordinal;
  return out.str();
}
} // namespace

std::string shortcutTargetId(const ShortcutConfig& shortcut, std::size_t legacyOrdinal) {
  const std::string_view identity = shortcut.id && Style::validMaterialTarget(*shortcut.id)
      ? std::string_view(*shortcut.id) : std::string_view{};
  return "control-center.home.shortcut." + std::string(
      identity.empty() ? legacyToken(shortcut.type, legacyOrdinal) : identity);
}

std::vector<std::string> shortcutSurfacePath(std::string_view targetId) {
  return {"panel", "control-center", "control-center.home", "control-center.home.shortcut", std::string(targetId)};
}

Style::MaterialTargetDescriptor descriptor(
    std::string id, std::string label, std::string family, std::vector<std::string> path
) {
  if (path.empty() || path.back() != id) path.push_back(id);
  return {.id = std::move(id), .label = std::move(label), .role = "surface",
          .family = std::move(family), .surfaces = std::move(path)};
}

void materializeShortcutIds(
    std::vector<ShortcutConfig>& shortcuts, const std::function<std::string()>& generateId
) {
  std::set<std::string, std::less<>> occupied;
  for (auto& shortcut : shortcuts) {
    if (shortcut.id && Style::validMaterialTarget(*shortcut.id) && occupied.insert(*shortcut.id).second) continue;
    do shortcut.id = generateId();
    while (!shortcut.id || !Style::validMaterialTarget(*shortcut.id) || occupied.contains(*shortcut.id));
    occupied.insert(*shortcut.id);
  }
}

} // namespace control_center_material
