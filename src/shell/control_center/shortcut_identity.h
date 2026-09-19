#pragma once

#include "config/config_types.h"
#include "ui/material_target_catalog.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace control_center_material {

[[nodiscard]] std::string shortcutTargetId(const ShortcutConfig& shortcut, std::size_t legacyOrdinal);
[[nodiscard]] std::vector<std::string> shortcutSurfacePath(std::string_view targetId);
[[nodiscard]] Style::MaterialTargetDescriptor descriptor(
    std::string id, std::string label, std::string family, std::vector<std::string> path
);

// Called only from an authorized Settings mutation. Existing IDs survive
// reorder; missing/colliding IDs receive fresh values from generateId.
void materializeShortcutIds(
    std::vector<ShortcutConfig>& shortcuts, const std::function<std::string()>& generateId
);

} // namespace control_center_material
