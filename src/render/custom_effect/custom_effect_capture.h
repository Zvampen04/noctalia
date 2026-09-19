#pragma once

#include "render/core/mat3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

struct CustomEffectCapture {
  int x = 0, y = 0, width = 0, height = 0;
  float axisXx = 0.0F, axisXy = 0.0F, axisYx = 0.0F, axisYy = 0.0F;
  bool operator==(const CustomEffectCapture&) const = default;
};

[[nodiscard]] inline std::optional<CustomEffectCapture> customEffectCapture(
    int viewportX, int viewportY, int viewportWidth, int viewportHeight,
    float surfaceWidth, float surfaceHeight, float width, float height,
    float sampleRadius, const Mat3& transform) noexcept {
  if (viewportWidth <= 0 || viewportHeight <= 0 || surfaceWidth <= 0.0F || surfaceHeight <= 0.0F
      || width <= 0.0F || height <= 0.0F || !std::isfinite(surfaceWidth) || !std::isfinite(surfaceHeight)
      || !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(sampleRadius)) return std::nullopt;
  for (const float value : transform.m) if (!std::isfinite(value)) return std::nullopt;
  const std::array<Vec2, 4> points{
      transform.transformPoint(0, 0), transform.transformPoint(width, 0),
      transform.transformPoint(0, height), transform.transformPoint(width, height)};
  float left=points[0].x,right=left,top=points[0].y,bottom=top;
  for (const auto& point : points) {
    left=std::min(left,point.x);right=std::max(right,point.x);
    top=std::min(top,point.y);bottom=std::max(bottom,point.y);
  }
  const float scaleX=static_cast<float>(viewportWidth)/surfaceWidth;
  const float scaleY=static_cast<float>(viewportHeight)/surfaceHeight;
  if (!std::isfinite(scaleX) || !std::isfinite(scaleY)) return std::nullopt;
  const float axisXx=transform.m[0]*scaleX,axisXy=-transform.m[1]*scaleY;
  const float axisYx=transform.m[3]*scaleX,axisYy=-transform.m[4]*scaleY;
  if (!std::isfinite(axisXx) || !std::isfinite(axisXy)
      || !std::isfinite(axisYx) || !std::isfinite(axisYy)) return std::nullopt;
  // The shader bounds offsets by their Euclidean local-space length. Expand by
  // the 2x2 transform's largest singular value: exact for identity/scale and
  // conservative for every direction under rotation, mirror, and shear.
  const double columnX2=static_cast<double>(axisXx)*axisXx+static_cast<double>(axisXy)*axisXy;
  const double columnY2=static_cast<double>(axisYx)*axisYx+static_cast<double>(axisYy)*axisYy;
  const double dot=static_cast<double>(axisXx)*axisYx+static_cast<double>(axisXy)*axisYy;
  const double discriminant=std::hypot(columnX2-columnY2,2.0*dot);
  const double stretch=std::sqrt(std::max(0.0,0.5*(columnX2+columnY2+discriminant)));
  const double physicalRadius=std::max(0.0,static_cast<double>(sampleRadius))*stretch;
  const int padding=static_cast<int>(std::ceil(std::clamp(
      physicalRadius,0.0,static_cast<double>(std::max(viewportWidth,viewportHeight)))));
  const int minX=viewportX,minY=viewportY,maxX=viewportX+viewportWidth,maxY=viewportY+viewportHeight;
  const int x=std::clamp(viewportX+static_cast<int>(std::floor(left*scaleX))-padding,minX,maxX);
  const int y=std::clamp(viewportY+viewportHeight-static_cast<int>(std::ceil(bottom*scaleY))-padding,minY,maxY);
  const int rightPx=std::clamp(viewportX+static_cast<int>(std::ceil(right*scaleX))+padding,minX,maxX);
  const int topPx=std::clamp(viewportY+viewportHeight-static_cast<int>(std::floor(top*scaleY))+padding,minY,maxY);
  if (rightPx <= x || topPx <= y) return std::nullopt;
  return CustomEffectCapture{.x=x,.y=y,.width=rightPx-x,.height=topPx-y,
      .axisXx=axisXx,.axisXy=axisXy,.axisYx=axisYx,.axisYy=axisYy};
}
