#pragma once

#include <array>
#include <cstddef>

// A continuous desktop frame with up to four shelves. Shelf rectangles are in
// local logical pixels; zero extents disable a shelf. The shader joins them to
// the inverse inner rectangle before computing coverage or material lighting.
struct FrameShelfContour {
  float x = 0, y = 0, width = 0, height = 0;
  float radius = 0, shoulder = 0;
  bool operator==(const FrameShelfContour&) const = default;
};

struct FrameContour {
  static constexpr std::size_t kShelfCapacity = 12;
  bool chamfered = false;
  std::array<float, 4> chamfers{}; // top-left, top-right, bottom-right, bottom-left
  std::array<FrameShelfContour, kShelfCapacity> shelves{};
  bool operator==(const FrameContour&) const = default;
};
