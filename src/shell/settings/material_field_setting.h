#pragma once

#include "shell/settings/settings_registry.h"

namespace settings {

inline SettingControl materialFieldSetting(std::string_view key, double value,
                                          double minimum, double maximum, double step) {
  if (key == "lens_mapping") {
    SelectSetting lens;
    lens.options = {{.value = "snell", .label = "Surface refraction"},
                    {.value = "radial", .label = "Concave lens"}};
    lens.selectedValue = value == 1 ? "radial" : "snell";
    lens.groupedCommit = [](std::string_view selected, const std::vector<std::string>& path) {
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> changes;
      if (selected == "snell") changes.emplace_back(path, 0.0);
      else if (selected == "radial") changes.emplace_back(path, 1.0);
      return changes;
    };
    return lens;
  }
  if (key != "optical_plane") return SliderSetting{value, minimum, maximum, step, false};
  SelectSetting choice;
  choice.options = {{.value = "automatic", .label = "Follow surface role"},
                    {.value = "inherited", .label = "Share parent glass"},
                    {.value = "independent", .label = "Own glass surface"}};
  choice.selectedValue = value == 0 ? "inherited" : value == 1 ? "independent" : "automatic";
  choice.groupedCommit = [](std::string_view selected, const std::vector<std::string>& path) {
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> changes;
    if (selected == "automatic") changes.emplace_back(path, -1.0);
    else if (selected == "inherited") changes.emplace_back(path, 0.0);
    else if (selected == "independent") changes.emplace_back(path, 1.0);
    return changes;
  };
  return choice;
}

inline std::string_view materialFieldDescription(std::string_view key) {
  if (key == "optical_plane")
    return "Share the parent glass surface or give this element its own refraction. Automatic follows its surface role.";
  if (key == "refraction_radius")
    return "Optical shape radius in logical pixels, independent of visible corners. -1 follows the painted shape; 0 uses a square optical field.";
  if (key == "lens_mapping")
    return "Choose shaped-surface refraction or radial concave-lens distortion. The visible surface outline stays unchanged.";
  if (key == "lens_strength")
    return "Strength of the concave lens, limited by maximum displacement. Zero removes radial distortion.";
  if (key == "lens_falloff")
    return "How the concave lens strength changes from the optical edge toward the center.";
  if (key == "rim_width")
    return "Rim width in pixels. Use -1 for automatic width or 0 to turn the rim off.";
  return "Controls how the current palette is rendered. Changes preview immediately.";
}

} // namespace settings
