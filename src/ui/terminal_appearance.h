#pragma once
#include <cstdint>
// Color remains owned by the palette engine. This policy only changes how a
// terminal renders its background, leaving glyphs fully opaque.
struct TerminalAppearance {
  bool enabled = false;
  float backgroundOpacity = 1.F;
  bool backgroundBlur = true;
  bool opacityCells = true;
  // Negative values retain application typography and spacing defaults.
  std::int32_t paddingX = -1;
  std::int32_t paddingY = -1;
  float fontSize = 0.F;
  bool operator==(const TerminalAppearance&) const = default;
};
