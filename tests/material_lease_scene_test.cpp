#include "render/custom_effect/custom_effect_types.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "wayland/material_scene.h"

#include <cassert>
#include <memory>
#include <string>

int main() {
  Node root;
  root.setFrameSize(100, 40);
  auto plane = std::make_unique<RectNode>();
  auto* planePtr = plane.get();
  plane->setFrameSize(80, 28);
  plane->setMaterialRole("popup");
  plane->setMaterialSurface("popup.context-menu");
  RoundedRectStyle style;
  style.fill = rgba(1, 1, 1);
  auto asset = std::make_shared<CustomEffectAsset>(CustomEffectAsset{
      .stableId = "user.lease-zero-opacity",
      .sha256Digest = std::string(64, 'a'),
      .source = "vec4 noctalia_effect(vec4 s,vec4 b,vec2 u,vec2 p,vec2 z,vec4 p0,vec4 p1,vec4 p2,vec4 p3,vec4 p4,vec4 p5,vec4 p6,vec4 p7){return s;}",
  });
  style.customBackground = CustomEffectBinding{.asset = std::move(asset)};
  plane->setStyle(style);
  root.addChild(std::move(plane));

  // Ordinary scene extraction remains paint-oriented. Lease extraction keeps
  // the same live target at opacity zero so the first visible frame cannot
  // switch from native to compositor ownership mid-entrance.
  planePtr->setOpacity(0);
  assert(collectMaterialScene(&root, 100, 40).planes.empty());
  const auto prewarmed = collectMaterialLeaseScene(&root, 100, 40);
  assert(prewarmed.planes.size() == 1);
  assert(prewarmed.planes[0].customEffect);
  assert(prewarmed.planes[0].opacity == 0);
  const auto stagedBytes = noctalia::material::encodeScene(
      prewarmed, noctalia::material::kLeasedSceneVersion);
  const auto staged = noctalia::material::decodeScene(stagedBytes);
  assert(staged && staged->leaseToken == 0);
  assert(staged->planes[0].customEffect && staged->planes[0].customEffect->staged);

  planePtr->setOpacity(0.5F);
  const auto opening = collectMaterialLeaseScene(&root, 100, 40);
  assert(opening.planes.size() == 1 && opening.planes[0].opacity == 0.5F);
  assert(opening.planes[0].group == prewarmed.planes[0].group);
  assert(opening.planes[0].surface == prewarmed.planes[0].surface);

  planePtr->setVisible(false);
  assert(collectMaterialLeaseScene(&root, 100, 40).planes.empty());
  return 0;
}
