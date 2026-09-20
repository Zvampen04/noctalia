#pragma once

#include "core/toml.h"
#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::profile {
using Path = std::vector<std::string>;

// Property ownership, independent of which settings page happens to display it.
// Palette, wallpaper, authentication, device state and key bindings stay outside
// an appearance recipe. Services retain their own persistence and actions.
inline bool owns(const Path& path) {
  if (path.empty()) return false;
  constexpr std::array roots{"bar", "dock", "widget", "desktop_widgets", "hot_corners", "osd"};
  if (std::ranges::find(roots, path[0]) != roots.end()) return true;
  // Layout and widget presentation participate in the appearance transaction.
  // The runtime enable gate remains lock/session policy and is never profile-owned.
  if (path[0] == "lockscreen_widgets" && path.size() >= 2) {
    constexpr std::array fields{"schema_version", "grid", "widget_order", "widget"};
    return std::ranges::find(fields, path[1]) != fields.end();
  }
  if (path.size() < 2) return false;
  if (path[0] == "shell") {
    constexpr std::array fields{
        "controls", "design", "desktop_frame", "material", "material_overrides", "surface_material", "font_family",
        "corner_radius_scale", "button_borders", "input_borders", "popup_borders",
        "popup_shadows", "card_borders", "panel", "launcher", "animation", "shadow",
        "screen_corners", "settings_window_translucent", "terminal_appearance", "settings_connected_rows", "settings_compact_chrome", "settings_window_width", "settings_window_height", "settings_background", "window_switcher", "panel_anchor_bar", "caret", "caret_app_integration", "caret_adopt_existing"};
    return std::ranges::find(fields, path[1]) != fields.end();
  }
  if (path[0] == "notification") {
    constexpr std::array fields{"background_opacity", "border", "scale", "position", "monitors",
        "width", "margin", "spacing", "max_visible", "timeout", "hover_pause", "progress"};
    return std::ranges::find(fields, path[1]) != fields.end();
  }
  if (path[0] == "control_center") {
    if (path[1] == "calendar") {
      constexpr std::array fields{"week_strip", "center_today", "fade_edges", "width", "height"};
      return path.size() == 3 && std::ranges::find(fields, path[2]) != fields.end();
    }
    if (path[1] == "media") {
      constexpr std::array fields{"layout", "artwork_size", "backdrop_opacity", "visualizer", "equalizer_access", "home_visibility", "width", "height"};
      return path.size() == 3 && std::ranges::find(fields, path[2]) != fields.end();
    }
    constexpr std::array fields{
        "shortcuts",          "sidebar",          "sidebar_section", "width",       "literal_width",
        "show_tray",          "compact_sections", "compact_height",  "hidden_tabs", "show_shortcut_labels",
        "show_session_button", "compact_columns", "compact_layout", "compact_navigation"
    };
    return std::ranges::find(fields, path[1]) != fields.end();
  }
  // Only declarative presentation of first-party panel entries belongs here;
  // device toggles, live values and the preset control mirror do not.
  if (path[0] == "plugin_settings" && path.size() >= 3) {
    const auto& key = path[2];
    return key.starts_with("panel_") || key.starts_with("shell_panel_");
  }
  return false;
}

inline toml::table subset(const toml::table& source, Path path = {}) {
  toml::table result;
  for (const auto& [key, value] : source) {
    auto child = path;
    child.emplace_back(key.str());
    if (owns(child)) {
      result.insert_or_assign(key, value);
    } else if (const auto* table = value.as_table()) {
      auto nested = subset(*table, std::move(child));
      if (!nested.empty()) result.insert_or_assign(key, std::move(nested));
    }
  }
  return result;
}

// Replace only owned properties, including deletion/reset. All other values in
// the destination remain current, so previews cannot revert a palette or device
// preference changed while settings is open.
inline void replace(toml::table& destination, const toml::table& source, Path path = {}) {
  std::vector<std::string> keys;
  for (const auto& [key, value] : destination) keys.emplace_back(key.str());
  for (const auto& [key, value] : source)
    if (std::ranges::find(keys, key.str()) == keys.end()) keys.emplace_back(key.str());
  for (const auto& key : keys) {
    auto child = path;
    child.push_back(key);
    if (owns(child)) {
      if (const auto* value = source.get(key)) destination.insert_or_assign(key, *value);
      else destination.erase(key);
      continue;
    }
    const auto* incoming = source[key].as_table();
    auto* current = destination[key].as_table();
    if (!incoming && !current) continue;
    if (!current) {
      auto owned = subset(*incoming, child);
      if (!owned.empty()) destination.insert_or_assign(key, std::move(owned));
      continue;
    }
    replace(*current, incoming ? *incoming : toml::table{}, std::move(child));
  }
}
// Merge unrelated edits against the last disk version. Conflicting edits of
// the same property fail instead of silently picking either writer.
inline bool rebaseUnrelated(toml::table& result, const toml::table& baseline,
    const toml::table& local, const toml::table& incoming, Path path = {}) {
  std::vector<std::string> keys;
  for (const auto* table : {&baseline, &local, &incoming})
    for (const auto& [key, value] : *table)
      if (std::ranges::find(keys, key.str()) == keys.end()) keys.emplace_back(key.str());
  const auto same = [](const toml::node* a, const toml::node* b) {
    if (!a || !b) return a == b;
    toml::table aa, bb;
    aa.insert("value", *a); bb.insert("value", *b);
    return aa == bb;
  };
  for (const auto& key : keys) {
    auto child = path; child.push_back(key);
    if (owns(child)) continue;
    const auto* base = baseline.get(key);
    const auto* ours = local.get(key);
    const auto* theirs = incoming.get(key);
    if (same(ours, base)) continue; // result already contains incoming.
    if ((!base || base->is_table()) && (!ours || ours->is_table()) && (!theirs || theirs->is_table())) {
      toml::table nested = theirs ? *theirs->as_table() : toml::table{};
      if (!rebaseUnrelated(nested, base ? *base->as_table() : toml::table{},
          ours ? *ours->as_table() : toml::table{}, theirs ? *theirs->as_table() : toml::table{}, child)) return false;
      if (nested.empty()) result.erase(key);
      else result.insert_or_assign(key, std::move(nested));
    } else {
      if (!same(theirs, base) && !same(ours, theirs)) return false;
      if (ours) result.insert_or_assign(key, *ours); else result.erase(key);
    }
  }
  return true;
}

} // namespace noctalia::profile
