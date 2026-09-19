#pragma once

#include "config/config_types.h"
#include "render/core/render_styles.h"

#include <algorithm>
#include <cmath>

namespace desktop_frame {

struct Geometry {
  struct BorderLayer { float width = 0, offset = 0; };
  RectInsets inset;
  float radius = 0;
  float borderWidth = 0;
  FrameContour contour;
  std::array<BorderLayer, 3> borderLayers{};
};

inline Geometry resolve(const DesktopFrameConfig& config, float width, float height) {
  Geometry result;
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) return result;
  const auto nonnegative = [](float value) { return std::isfinite(value) ? std::max(0.F, value) : 0.F; };
  const float scale = config.referenceWidth > 0 && config.referenceHeight > 0
      ? std::clamp(std::min(width/std::max(1.F,config.referenceWidth),
                           height/std::max(1.F,config.referenceHeight)),0.F,16384.F) : 1.F;
  // Keep a nonnegative aperture even on tiny outputs. Opposite inset proportions
  // survive fitting rather than letting one side consume the other arbitrarily.
  float left=nonnegative(config.left)*scale, right=nonnegative(config.right)*scale;
  float top=nonnegative(config.top)*scale, bottom=nonnegative(config.bottom)*scale;
  const float fitX=std::min(1.F,width/std::max(1.F,left+right));
  const float fitY=std::min(1.F,height/std::max(1.F,top+bottom));
  left*=fitX; right*=fitX; top*=fitY; bottom*=fitY;
  result.inset={left,top,right,bottom};
  result.borderWidth=std::min(nonnegative(config.borderWidth)*scale,std::min(width,height));
  result.radius=std::min(nonnegative(config.radius)*scale,std::max(0.F,std::min(width-left-right,height-top-bottom))*0.5F);
  result.contour.chamfered=config.chamfered;
  const float chamferLimit=std::max(0.F,std::min(width-left-right,height-top-bottom))*.5F;
  const float chamfers[]={config.chamferTopLeft,config.chamferTopRight,config.chamferBottomRight,config.chamferBottomLeft};
  for (std::size_t i=0;i<4;++i) result.contour.chamfers[i]=std::min(nonnegative(chamfers[i])*scale,chamferLimit);
  const auto resolveShelf = [&](const auto& shelf, std::size_t edge, std::size_t destination) {
    if (!shelf.enabled || !std::isfinite(shelf.start) || !std::isfinite(shelf.end)) return;
    const bool vertical=edge==0 || edge==2;
    const float length=vertical?height:width;
    const float start=std::clamp(shelf.start,0.F,1.F)*length;
    const float end=std::clamp(shelf.end,0.F,1.F)*length;
    if (end<=start) return;
    const float depth=std::min(nonnegative(shelf.depth)*scale,vertical?width:height);
    if (depth<=0) return;
    FrameShelfContour shape;
    if (edge==0) shape={0,start,std::min(width,left+depth),end-start};
    if (edge==1) shape={start,0,end-start,std::min(height,top+depth)};
    if (edge==2) { const float w=std::min(width,right+depth); shape={width-w,start,w,end-start}; }
    if (edge==3) { const float h=std::min(height,bottom+depth); shape={start,height-h,end-start,h}; }
    shape.radius=std::min(nonnegative(shelf.radius)*scale,std::min(shape.width,shape.height)*0.5F);
    shape.shoulder=std::min(nonnegative(shelf.shoulder)*scale,std::min(width,height)*0.5F);
    result.contour.shelves[destination]=shape;
  };
  const DesktopFrameShelfConfig* legacyShelves[]={&config.leftShelf,&config.topShelf,&config.rightShelf,&config.bottomShelf};
  for (std::size_t i=0;i<4;++i) resolveShelf(*legacyShelves[i],i,i);
  for (std::size_t i=0;i<config.shelves.size();++i)
    resolveShelf(config.shelves[i],static_cast<std::size_t>(config.shelves[i].edge),i+4);
  for (std::size_t i=0;i<config.borderLayers.size();++i) {
    const auto& layer=config.borderLayers[i];
    if (!layer.enabled) continue;
    const float limit=std::min(width,height);
    result.borderLayers[i].offset=std::min(nonnegative(layer.offset)*scale,limit);
    result.borderLayers[i].width=std::min(nonnegative(layer.width)*scale,
        std::max(0.F,limit-result.borderLayers[i].offset));
  }
  return result;
}

} // namespace desktop_frame
