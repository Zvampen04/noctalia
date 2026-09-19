#pragma once
#include "shell/panel/attached_panel_context.h"
#include <algorithm>
#include <cmath>

namespace attached_panel {
struct MorphRect { float x=0,y=0,width=0,height=0; };
struct MorphSettings {
  float startWidth=.55F;
  float cornerGrowth=.5F;
  float contentTravel=.1F;
};
struct MorphGeometry {
  MorphRect body, background, contentClip;
  float radius=0, contentX=0,contentY=0;
  Radii radii{};
};
inline MorphGeometry morphGeometry(MorphRect finalBody, float fullRadius,
    AttachedRevealDirection direction,float progress,MorphSettings settings={}) {
  const auto bounded=[](float v,float low,float high,float fallback) {
    return std::clamp(std::isfinite(v)?v:fallback,low,high);
  };
  const float t=bounded(progress,0,1,0);
  const float seed=bounded(settings.startWidth,0,1,.55F);
  const float growth=bounded(settings.cornerGrowth,.1F,2,.5F);
  const float travel=bounded(settings.contentTravel,0,1,.1F);
  finalBody.width=std::max(0.F,finalBody.width);
  finalBody.height=std::max(0.F,finalBody.height);
  const bool vertical=direction==AttachedRevealDirection::Left || direction==AttachedRevealDirection::Right;
  MorphGeometry result;result.body=finalBody;
  const float cross=seed+(1-seed)*t;
  result.body.width*=vertical?t:cross;
  result.body.height*=vertical?cross:t;
  if (vertical) result.body.y+=(finalBody.height-result.body.height)*.5F;
  else result.body.x+=(finalBody.width-result.body.width)*.5F;
  if (direction==AttachedRevealDirection::Left) result.body.x+=finalBody.width-result.body.width;
  if (direction==AttachedRevealDirection::Up) result.body.y+=finalBody.height-result.body.height;
  result.radius=std::min({std::max(0.F,fullRadius)*std::pow(t,growth),result.body.width*.5F,result.body.height*.5F});
  result.radii={result.radius,result.radius,result.radius,result.radius};
  result.background=result.body;
  if (vertical) {result.background.y-=result.radius;result.background.height+=2*result.radius;}
  else {result.background.x-=result.radius;result.background.width+=2*result.radius;}
  // Largest simple centered corner-safe inset. Normal panel padding exceeds this
  // at full size; during growth it prevents foreground escaping the silhouette.
  const float guard=result.radius*(1.F-std::sqrt(.5F));
  result.contentClip={result.body.x+guard,result.body.y+guard,
      std::max(0.F,result.body.width-2*guard),std::max(0.F,result.body.height-2*guard)};
  const float offset=(vertical?finalBody.width:finalBody.height)*travel*(1-t);
  if (direction==AttachedRevealDirection::Down) result.contentY=-offset;
  if (direction==AttachedRevealDirection::Up) result.contentY=offset;
  if (direction==AttachedRevealDirection::Right) result.contentX=-offset;
  if (direction==AttachedRevealDirection::Left) result.contentX=offset;
  return result;
}

// A detached island owns its whole silhouette, including the compact opener.
// Progress has already been eased by the one panel animation clock.
inline MorphGeometry islandMorphGeometry(MorphRect source, Radii sourceRadii,
    MorphRect finalBody, float fullRadius, float progress) {
  const float t=std::clamp(std::isfinite(progress)?progress:0.F,0.F,1.F);
  const auto mix=[t](float a,float b){return std::lerp(a,b,t);};
  MorphGeometry result;
  result.body={mix(source.x,finalBody.x),mix(source.y,finalBody.y),
      std::max(0.F,mix(source.width,finalBody.width)),std::max(0.F,mix(source.height,finalBody.height))};
  result.background=result.body;
  const float limit=std::min(result.body.width,result.body.height)*.5F;
  const auto radius=[&](float r){return std::clamp(mix(std::isfinite(r)?r:0.F,
      std::isfinite(fullRadius)?fullRadius:0.F),0.F,limit);};
  result.radii={radius(sourceRadii.tl),radius(sourceRadii.tr),radius(sourceRadii.br),radius(sourceRadii.bl)};
  result.radius=std::max({result.radii.tl,result.radii.tr,result.radii.br,result.radii.bl});
  const float guard=result.radius*(1.F-std::sqrt(.5F));
  result.contentClip={result.body.x+guard,result.body.y+guard,
      std::max(0.F,result.body.width-2*guard),std::max(0.F,result.body.height-2*guard)};
  // Translate retained content without changing font, icon or hitbox scale.
  result.contentX=result.body.x-finalBody.x;
  result.contentY=result.body.y-finalBody.y;
  return result;
}
}
