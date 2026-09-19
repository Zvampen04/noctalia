#pragma once
#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <type_traits>

namespace Style {
enum class ButtonTreatment { Standard, RaisedInset };
enum class ToggleTreatment { Thumb, Sweep };
enum class CheckboxTreatment { Check, Plateau };
enum class CardTreatment { Standard, Raised };
enum class SegmentedTreatment { Standard, Floating };
enum class ControlPaletteRole { Surface, SurfaceVariant, Primary, Secondary, OnSurface, OnPrimary };
template<class T> constexpr auto controlKeys() {
  if constexpr (std::is_same_v<T, ButtonTreatment>) return std::array<std::string_view, 2>{"standard", "raised_inset"};
  else if constexpr (std::is_same_v<T, ToggleTreatment>) return std::array<std::string_view, 2>{"thumb", "sweep"};
  else if constexpr (std::is_same_v<T, CheckboxTreatment>) return std::array<std::string_view, 2>{"check", "plateau"};
  else if constexpr (std::is_same_v<T, CardTreatment>) return std::array<std::string_view, 2>{"standard", "raised"};
  else if constexpr (std::is_same_v<T, SegmentedTreatment>) return std::array<std::string_view, 2>{"standard", "floating"};
  else return std::array<std::string_view, 6>{"surface", "surface_variant", "primary", "secondary", "on_surface", "on_primary"};
}
template<class T> constexpr std::string_view controlName(T value) {
  const auto keys = controlKeys<T>();
  const auto index = static_cast<std::size_t>(value);
  return index < keys.size() ? keys[index] : std::string_view{};
}
template<class T> constexpr std::optional<T> parseControl(std::string_view value) {
  const auto keys = controlKeys<T>();
  for (std::size_t index = 0; index < keys.size(); ++index)
    if (keys[index] == value) return static_cast<T>(index);
  return std::nullopt;
}
struct ControlSettings {
#define CONTROL_ENUM(member, type, initial, label, group) type member = type::initial;
#define CONTROL_NUMBER(member, initial, low, high, step, label, group) float member = initial;
#define CONTROL_BOOL(member, initial, label, group) bool member = initial;
#include "ui/control_settings.def"
#undef CONTROL_ENUM
#undef CONTROL_NUMBER
#undef CONTROL_BOOL
  bool operator==(const ControlSettings&) const = default;
};
struct SweepGeometry {
  float width, height, indicatorWidth, indicatorHeight, x, y;
};
inline SweepGeometry sweepGeometry(const ControlSettings& settings, float scale, float progress, bool rtl) {
  const float height = settings.toggle_height * scale;
  const float width = height * settings.toggle_width_ratio;
  const float inset = settings.toggle_indicator_inset * scale;
  const float indicatorWidth = std::max(0.0F, width * settings.toggle_indicator_width_ratio - 2 * inset);
  const float indicatorHeight = std::max(0.0F, height * settings.toggle_indicator_height_ratio - 2 * inset);
  const float travel = settings.toggle_mirror_rtl && rtl ? 1.0F - progress : progress;
  const float position = settings.toggle_indicator_off +
      (settings.toggle_indicator_on - settings.toggle_indicator_off) * travel;
  return {width, height, indicatorWidth, indicatorHeight, inset + position * indicatorWidth,
          (height - indicatorHeight) * 0.5F};
}
inline float controlBezier(float position, float x1, float y1, float x2, float y2) {
  position = std::clamp(position, 0.0F, 1.0F);
  const auto coordinate = [](float t, float first, float second) {
    const float one = 1.0F - t;
    return 3.0F * one * one * t * first + 3.0F * one * t * t * second + t * t * t;
  };
  float low = 0.0F, high = 1.0F;
  for (int count = 0; count < 16; ++count) {
    const float middle = (low + high) * 0.5F;
    if (coordinate(middle, x1, x2) < position) low = middle;
    else high = middle;
  }
  return position == 0.0F || position == 1.0F ? position :
      coordinate((low + high) * 0.5F, y1, y2);
}
inline float sweepCurve(float position, const ControlSettings& settings) {
  return controlBezier(position, settings.toggle_curve_x1, settings.toggle_curve_y1,
                       settings.toggle_curve_x2, settings.toggle_curve_y2);
}
}
