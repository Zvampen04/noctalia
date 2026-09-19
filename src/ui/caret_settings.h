#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>

enum class CaretShape { Bar, Block, Underline };

inline std::optional<CaretShape> parseCaretShape(std::string_view value) {
  if (value == "bar") return CaretShape::Bar;
  if (value == "block") return CaretShape::Block;
  if (value == "underline") return CaretShape::Underline;
  return std::nullopt;
}

inline std::string_view caretShapeName(CaretShape value) {
  switch (value) {
  case CaretShape::Block: return "block";
  case CaretShape::Underline: return "underline";
  default: return "bar";
  }
}

struct CaretSettings {
  CaretShape shape = CaretShape::Bar;
  float widthPx = 1.25F;
  bool blink = true;
  float blinkIntervalMs = 530.0F;
  float motionMs = 0.0F;
  bool operator==(const CaretSettings&) const = default;
};

inline CaretSettings sanitizedCaretSettings(CaretSettings value) {
  const auto bounded = [](float v, float fallback, float low, float high) {
    return std::isfinite(v) ? std::clamp(v, low, high) : fallback;
  };
  // Also normalize an invalid enum passed by a native caller.
  if (value.shape != CaretShape::Bar && value.shape != CaretShape::Block && value.shape != CaretShape::Underline)
    value.shape = CaretShape::Bar;
  value.widthPx = bounded(value.widthPx, 1.25F, 1.0F, 8.0F);
  value.blinkIntervalMs = bounded(value.blinkIntervalMs, 530.0F, 100.0F, 2000.0F);
  value.motionMs = bounded(value.motionMs, 0.0F, 0.0F, 400.0F);
  return value;
}
