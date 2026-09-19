#pragma once

#include "config/config_types.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "core/toml.h"
#include "ui/palette.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace greeter::detail {

inline const std::set<std::string>& lockWidgetSettings(std::string_view type) {
  static const std::set<std::string> kCommon{
      "background", "background_color", "background_opacity", "background_radius", "background_padding"};
  static const std::set<std::string> kLoginBox{
      "layout", "background_color", "background_opacity", "background_radius", "input_opacity", "input_radius",
      "center_password_text"};
  static const std::set<std::string> kClock{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "clock_style", "format", "center_text", "timezone", "color", "font_family", "shadow", "circle"};
  static const std::set<std::string> kCalendar{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "show_events", "show_week_numbers", "font_family"};
  static const std::set<std::string> kLabel{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "title", "description", "color", "opacity", "font_family", "shadow"};
  static const std::set<std::string> kWeather{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "color", "font_family", "shadow", "show_forecast", "forecast_days"};
  static const std::set<std::string> kMedia{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "layout", "color", "font_family", "shadow", "hide_when_no_media"};
  static const std::set<std::string> kSysmon{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "stat", "stat2", "network_speed_unit", "network_speed_compact", "display", "gauge_layout", "color", "color2",
      "highlight_color", "font_family", "show_label", "label_min_width", "shadow"};
  static const std::set<std::string> kVolume{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "glyph", "fill_color", "track_color", "show_device", "font_family", "shadow"};
  static const std::set<std::string> kStatus{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "color", "show_label", "font_family"};
  static const std::set<std::string> kAudio{
      "background", "background_color", "background_opacity", "background_radius", "background_padding", "bands",
      "mirrored", "reversed", "centered", "show_when_idle", "gap_ratio", "corner_radius", "reflection_height",
      "reflection_opacity", "smoothing_ms", "respect_global_motion", "color_1", "color_2"};
  static const std::set<std::string> kFancyAudio{
      "background", "background_color", "background_opacity", "background_radius", "background_padding",
      "visualization_mode", "sensitivity", "rotation_speed", "bar_width", "wave_thickness", "ring_opacity",
      "inner_diameter", "bloom_intensity", "fade_when_idle", "respect_global_motion", "primary_color", "secondary_color"};
  if (type == "login_box") return kLoginBox;
  if (type == "clock") return kClock;
  if (type == "calendar") return kCalendar;
  if (type == "label") return kLabel;
  if (type == "weather") return kWeather;
  if (type == "media_player") return kMedia;
  if (type == "sysmon") return kSysmon;
  if (type == "volume") return kVolume;
  if (type == "battery" || type == "brightness") return kStatus;
  if (type == "audio_visualizer") return kAudio;
  if (type == "fancy_audio_visualizer") return kFancyAudio;
  return kCommon;
}

inline bool lockWidgetTypeAllowed(std::string_view type) {
  constexpr std::array allowed{
      "login_box", "clock", "calendar", "label", "weather", "media_player", "sysmon", "volume", "battery",
      "brightness", "audio_visualizer", "fancy_audio_visualizer"};
  return std::ranges::find(allowed, type) != allowed.end();
}

inline bool lockWidgetToken(std::string_view value, std::size_t maximum, std::string_view extra = {}) {
  return !value.empty() && value.size() <= maximum && std::ranges::all_of(value, [extra](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '_' || c == '-' || c == '.' || extra.contains(static_cast<char>(c));
  });
}

inline void insertScalarSetting(toml::table& target, const std::string& key, const WidgetSettingValue& value) {
  std::visit([&](const auto& concrete) {
    using T = std::decay_t<decltype(concrete)>;
    if constexpr (std::is_same_v<T, double>) {
      if (std::isfinite(concrete) && concrete >= -65536 && concrete <= 65536) target.insert_or_assign(key, concrete);
    } else if constexpr (std::is_same_v<T, std::string>) {
      if (concrete.size() <= 256 && std::ranges::none_of(concrete, [](unsigned char c) { return c < 32 && c != '\n'; }))
        target.insert_or_assign(key, concrete);
    } else if constexpr (std::is_same_v<T, bool>) {
      target.insert_or_assign(key, concrete);
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
      if (concrete >= -65536 && concrete <= 65536) target.insert_or_assign(key, concrete);
    }
  }, value);
}

inline toml::table lockWidgetLayoutSnapshot(const LockscreenWidgetsConfig& layout) {
  toml::table result;
  result.insert("version", std::int64_t{1});
  toml::array widgets;
  std::size_t count = 0;
  std::set<std::string> ids;
  for (const auto& widget : layout.widgets) {
    if (count >= 64 || !lockWidgetTypeAllowed(widget.type) || !lockWidgetToken(widget.id, 64, "@")
        || !ids.insert(widget.id).second
        || (!widget.outputName.empty() && !lockWidgetToken(widget.outputName, 128, ":@"))
        || !std::isfinite(widget.cx) || widget.cx < -65536 || widget.cx > 65536
        || !std::isfinite(widget.cy) || widget.cy < -65536 || widget.cy > 65536
        || !std::isfinite(widget.placementWidth) || widget.placementWidth < 0 || widget.placementWidth > 16384
        || !std::isfinite(widget.placementHeight) || widget.placementHeight < 0 || widget.placementHeight > 16384
        || !std::isfinite(widget.boxWidth) || widget.boxWidth < 0 || widget.boxWidth > 16384
        || !std::isfinite(widget.boxHeight) || widget.boxHeight < 0 || widget.boxHeight > 16384
        || !std::isfinite(widget.rotationRad) || widget.rotationRad < -6.28318530718F
        || widget.rotationRad > 6.28318530718F) continue;
    toml::table item;
    item.insert("id", widget.id);
    item.insert("type", widget.type);
    item.insert("output", widget.outputName);
    item.insert("cx", static_cast<double>(widget.cx));
    item.insert("cy", static_cast<double>(widget.cy));
    item.insert("placement_width", static_cast<double>(widget.placementWidth));
    item.insert("placement_height", static_cast<double>(widget.placementHeight));
    item.insert("box_width", static_cast<double>(widget.boxWidth));
    item.insert("box_height", static_cast<double>(widget.boxHeight));
    item.insert("rotation", static_cast<double>(widget.rotationRad));
    item.insert("flip_x", widget.flipX);
    item.insert("flip_y", widget.flipY);
    item.insert("enabled", widget.enabled);
    toml::table settings;
    const auto& allowed = lockWidgetSettings(widget.type);
    std::vector<std::string> keys;
    for (const auto& [key, ignored] : widget.settings) if (allowed.contains(key)) keys.push_back(key);
    std::ranges::sort(keys);
    for (const auto& key : keys) insertScalarSetting(settings, key, widget.settings.at(key));
    item.insert("settings", std::move(settings));
    widgets.push_back(std::move(item));
    ++count;
  }
  result.insert("widgets", std::move(widgets));
  return result;
}

// Shared by live user-session previews and the separately installed login
// appearance. Explicit renderer-only ownership prevents service/auth data from
// entering either output. Config and palette are snapshots from the UI thread.
inline toml::table appearanceSnapshot(const ShellConfig& shell, const Palette& colors) {
  namespace schema = noctalia::config::schema;
  const auto settings = schema::writeTable(shell, schema::shellSchema());
  toml::table result;
  result.insert("version", std::int64_t{1});
  for (const auto key : {"design", "material", "material_overrides", "controls", "caret", "font_family", "surface_material",
                         "corner_radius_scale", "animation", "button_borders", "input_borders",
                         "card_borders", "popup_borders", "popup_shadows"}) {
    if (const auto* value = settings.get(key)) result.insert(key, *value);
  }
  toml::table palette;
  for (const auto& [key, member] : std::array{
           std::pair{"primary", &Palette::primary}, std::pair{"on_primary", &Palette::onPrimary},
           std::pair{"secondary", &Palette::secondary}, std::pair{"on_secondary", &Palette::onSecondary},
           std::pair{"tertiary", &Palette::tertiary}, std::pair{"on_tertiary", &Palette::onTertiary},
           std::pair{"error", &Palette::error}, std::pair{"on_error", &Palette::onError},
           std::pair{"surface", &Palette::surface}, std::pair{"on_surface", &Palette::onSurface},
           std::pair{"surface_variant", &Palette::surfaceVariant}, std::pair{"on_surface_variant", &Palette::onSurfaceVariant},
           std::pair{"outline", &Palette::outline}, std::pair{"shadow", &Palette::shadow},
           std::pair{"hover", &Palette::hover}, std::pair{"on_hover", &Palette::onHover}}) {
    palette.insert(key, formatRgbHex(colors.*member));
  }
  result.insert("palette", std::move(palette));
  return result;
}

inline toml::table appearanceSnapshot(const Config& config, const Palette& colors) {
  auto result = appearanceSnapshot(config.shell, colors);
  result.insert("lock_widgets", lockWidgetLayoutSnapshot(config.lockscreenWidgets));
  return result;
}

inline std::string appearanceSnapshotJson(const ShellConfig& shell, const Palette& colors) {
  std::ostringstream out;
  out << toml::json_formatter{appearanceSnapshot(shell, colors)};
  return out.str();
}


inline std::string appearanceSnapshotJson(const Config& config, const Palette& colors) {
  std::ostringstream out;
  out << toml::json_formatter{appearanceSnapshot(config, colors)};
  return out.str();
}

} // namespace greeter::detail
