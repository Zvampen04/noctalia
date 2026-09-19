#pragma once

#include "shell/panel/attached_panel_context.h"
#include <algorithm>
#include <cmath>
#include <optional>

namespace attached_panel {
struct BodyRect { int x, y, width, height; };

inline BodyRect includeIslandSource(BodyRect body, const AttachedPanelSource& source,
                                   int outputWidth, int outputHeight) {
  if (!source.valid() || outputWidth<=0 || outputHeight<=0) return body;
  const auto bound=[](double value,int extent){return std::clamp(value,0.0,static_cast<double>(extent));};
  const int left=static_cast<int>(std::floor(bound(std::min<double>(body.x,source.x),outputWidth)));
  const int top=static_cast<int>(std::floor(bound(std::min<double>(body.y,source.y),outputHeight)));
  const int right=static_cast<int>(std::ceil(bound(std::max(static_cast<double>(body.x)+body.width,
      static_cast<double>(source.x)+source.width),outputWidth)));
  const int bottom=static_cast<int>(std::ceil(bound(std::max(static_cast<double>(body.y)+body.height,
      static_cast<double>(source.y)+source.height),outputHeight)));
  return {left,top,std::max(1,right-left),std::max(1,bottom-top)};
}

// Independent islands use the requested outer size, rather than adding the
// bar thickness. Side islands expand outward; the central island stays centered.
// The source remains available to the shared morph paint/input geometry.
inline BodyRect fitOutwardIsland(AttachedRevealDirection direction, BodyRect bar,
                                const AttachedPanelSource& source, int outputWidth, int outputHeight,
                                int width, int height, int padding, int offset) {
  const bool vertical = direction == AttachedRevealDirection::Right || direction == AttachedRevealDirection::Left;
  const bool positive = direction == AttachedRevealDirection::Right || direction == AttachedRevealDirection::Down;
  outputWidth = std::max(1, outputWidth); outputHeight = std::max(1, outputHeight);
  const int padX = std::clamp(padding, 0, (outputWidth - 1) / 2);
  const int padY = std::clamp(padding, 0, (outputHeight - 1) / 2);
  width = std::clamp(width, 1, outputWidth - 2 * padX);
  height = std::clamp(height, 1, outputHeight - 2 * padY);
  const float center = vertical ? source.y + source.height / 2 : source.x + source.width / 2;
  const int size = vertical ? height : width;
  const int thickness = vertical ? bar.width : bar.height;
  float cross = center - size / 2.F;
  if (source.section == AttachedPanelSourceSection::Start) cross = center + thickness / 2.F - offset - size;
  if (source.section == AttachedPanelSourceSection::End) cross = center - thickness / 2.F + offset;
  const int edge = vertical ? bar.x : bar.y;
  const int main = positive ? edge + offset : edge + thickness - offset - (vertical ? width : height);
  const int x = vertical ? main : static_cast<int>(std::lround(cross));
  const int y = vertical ? static_cast<int>(std::lround(cross)) : main;
  return {std::clamp(x, padX, outputWidth - padX - width),
          std::clamp(y, padY, outputHeight - padY - height), width, height};
}

// Fit the body into the output while keeping its bar-facing edge attached.
// A short dock may be narrower than its panel; output bounds win in that case.
inline BodyRect fitBody(AttachedRevealDirection direction, BodyRect bar,
                        int outputWidth, int outputHeight, int preferredWidth, int preferredHeight,
                        int padding, int overlap, int startInset, int endInset,
                        std::optional<float> crossAnchor = {}) {
  outputWidth = std::max(1, outputWidth);
  outputHeight = std::max(1, outputHeight);
  const bool vertical = direction == AttachedRevealDirection::Right || direction == AttachedRevealDirection::Left;
  const bool positive = direction == AttachedRevealDirection::Right || direction == AttachedRevealDirection::Down;
  const int crossOutput = vertical ? outputHeight : outputWidth;
  const int mainOutput = vertical ? outputWidth : outputHeight;
  const int crossPad = std::clamp(padding, 0, (crossOutput - 1) / 2);
  const int mainPad = std::clamp(padding, 0, (mainOutput - 1) / 2);
  const int crossStart = vertical ? bar.y : bar.x;
  const int crossExtent = vertical ? bar.height : bar.width;
  const int edge = std::clamp((vertical ? bar.x : bar.y)
      + (positive ? (vertical ? bar.width : bar.height) - overlap : overlap), 0, mainOutput);
  const int crossSize = std::clamp(vertical ? preferredHeight : preferredWidth, 1, crossOutput - 2 * crossPad);
  const int mainRoom = positive ? mainOutput - mainPad - edge : edge - mainPad;
  const int mainSize = std::clamp(vertical ? preferredWidth : preferredHeight, 1, std::max(1, mainRoom));
  const int desired = static_cast<int>(std::lround(crossAnchor.value_or(crossStart + crossExtent * .5F) - crossSize * .5F));
  int low = std::max(crossPad, crossStart + std::max(0, startInset));
  int high = std::min(crossOutput - crossPad - crossSize, crossStart + crossExtent - crossSize - std::max(0, endInset));
  if (low > high) { low = crossPad; high = crossOutput - crossPad - crossSize; }
  const int cross = std::clamp(desired, low, high);
  const int main = std::clamp(positive ? edge : edge - mainSize, 0, mainOutput - mainSize);
  return vertical ? BodyRect{main, cross, mainSize, crossSize} : BodyRect{cross, main, crossSize, mainSize};
}
// The compositor may accept less space than requested. Keep all scene geometry
// inside that accepted extent; right/bottom anchoring moves the surface origin.
struct ConfiguredBody {
  BodyRect outputBody;
  int insetX, insetY, trailingX, trailingY;
};
inline ConfiguredBody configuredBody(BodyRect requested, int insetX, int insetY, int trailingX, int trailingY,
                                    int requestedWidth, int requestedHeight, int width, int height,
                                    bool anchoredRight, bool anchoredBottom) {
  width = std::max(1, width); height = std::max(1, height);
  const int x = std::clamp(insetX, 0, width - 1);
  const int y = std::clamp(insetY, 0, height - 1);
  const int right = std::clamp(trailingX, 0, width - x - 1);
  const int bottom = std::clamp(trailingY, 0, height - y - 1);
  const int originX = requested.x - insetX + (anchoredRight ? requestedWidth - width : 0);
  const int originY = requested.y - insetY + (anchoredBottom ? requestedHeight - height : 0);
  return {{originX + x, originY + y, std::clamp(requested.width, 1, width - x - right),
           std::clamp(requested.height, 1, height - y - bottom)}, x, y, right, bottom};
}
} // namespace attached_panel
