#pragma once

#include "shell/settings/settings_registry.h"
#include <algorithm>
#include <cmath>

namespace settings {
// Named editing shortcuts for generic material parameters, never renderer modes.
inline SelectSetting materialShapeSetting(float elevation, float curvature,
                                          std::vector<std::string> elevationPath) {
  SelectSetting shape;
  shape.options = {{.value="flat", .label="Flat face"},
                   {.value="concave", .label="Concave"},
                   {.value="convex", .label="Convex"},
                   {.value="pressed", .label="Pressed"},
                   {.value="custom", .label="Custom"}};
  shape.selectedValue = elevation == 0 ? "custom" :
      curvature == 0 ? (elevation < 0 ? "pressed" : "flat") :
      elevation > 0 && curvature == -1 ? "concave" :
      elevation > 0 && curvature == 1 ? "convex" : "custom";
  shape.linkedPath = elevationPath;
  shape.groupedCommit = [elevation, elevationPath = std::move(elevationPath)](
      std::string_view value, const std::vector<std::string>& curvaturePath) {
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> changes;
    if (value != "flat" && value != "concave" && value != "convex" && value != "pressed") return changes;
    const double height = elevation == 0 ? 1.0 : std::abs(static_cast<double>(elevation));
    changes.emplace_back(elevationPath, value == "pressed" ? -height : height);
    changes.emplace_back(curvaturePath, value == "concave" ? -1.0 : value == "convex" ? 1.0 : 0.0);
    return changes;
  };
  return shape;
}
}
