#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "wayland/material_scene.h"
#include "ui/surface_material.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "render/scene/image_node.h"

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
  std::exit(EXIT_FAILURE); } } while (false)

int main() {
  using noctalia::material::Parameters;
  using noctalia::material::Primitive;
  Node::setDefaultCornerPower(2);
  Node bar, dock;
  bar.setCornerPower(4); dock.setCornerPower(10);
  bar.setMaterialSurface("bar");
  dock.setMaterialSurface("dock");
  auto group = std::make_unique<Node>();
  auto rectangle = std::make_unique<RectNode>();
  auto* rect = rectangle.get();
  const auto seed = rect->materialSeed();
  RoundedRectStyle style;
  style.material = Parameters{};
  rect->setStyle(style);
  rect->setMaterialResolver([](std::string_view surface) {
    Parameters result;
    if (surface == "bar") result.primitive = Primitive::Optical;
    else if (surface == "dock") result.primitive = Primitive::Plateau;
    else if (surface == "osd") result.primitive = Primitive::Illustrated;
    return result;
  });
  group->addChild(std::move(rectangle));
  CHECK(rect->materialSurfaceName().empty());
  CHECK(rect->style().material->primitive == Primitive::Flat);

  auto* attached = bar.addChild(std::move(group));
  CHECK(rect->materialSurfaceName() == "bar");
  CHECK(rect->cornerPower()==4);
  CHECK(rect->style().material->primitive == Primitive::Optical);

  auto moved = bar.removeChild(attached);
  CHECK(rect->materialSurfaceName().empty());
  CHECK(rect->style().material->primitive == Primitive::Flat);
  attached = dock.addChild(std::move(moved));
  CHECK(rect->materialSurfaceName() == "dock");
  CHECK(rect->cornerPower()==10);
  CHECK(rect->style().material->primitive == Primitive::Plateau);

  attached->setMaterialSurface("osd");
  CHECK(rect->materialSurfaceName() == "osd");
  CHECK(rect->style().material->primitive == Primitive::Illustrated);
  dock.setMaterialSurface("bar");
  CHECK(rect->materialSurfaceName() == "osd");
  CHECK(rect->style().material->primitive == Primitive::Illustrated);
  attached->setMaterialSurface("");
  CHECK(rect->materialSurfaceName() == "bar");
  CHECK(rect->style().material->primitive == Primitive::Optical);

  // A later child inserted into an already-scoped subtree inherits immediately.
  auto late = std::make_unique<RectNode>();
  auto* lateRect = late.get();
  late->setStyle(style);
  late->setMaterialResolver([](std::string_view surface) {
    Parameters result;
    result.plateau.elevation = surface == "bar" ? -2.0F : 3.5F;
    return result;
  });
  attached->insertChildAt(0, std::move(late));
  CHECK(lateRect->style().material->plateau.elevation == -2.0F);
  CHECK(rect->materialSeed() == seed);
  CHECK(lateRect->materialSeed() != seed);
  // Own-background clips bypass exactly one container. Independent controls
  // remain separate optical planes with the same local rounded mask as native paint.
  Node root;root.setFrameSize(200,120);root.setClipChildren(true);
  auto viewport=std::make_unique<Node>();viewport->setFrameSize(100,40);viewport->setPosition(50,40);viewport->setClipChildren(true);
  auto background=std::make_unique<RectNode>();background->setFrameSize(100,40);background->setBypassParentPaintClip(true);
  RoundedRectStyle glass;glass.fill=rgba(1,1,1);glass.material=Parameters{};
  glass.material->primitive=Primitive::Optical;glass.materialPlane=true;
  background->setStyle(glass);viewport->addChild(std::move(background));
  auto control=std::make_unique<RectNode>();auto* controlPtr=control.get();
  control->setFrameSize(30,30);control->setPosition(-10,-10);
  glass.materialPlane=false;glass.material->optical.planeMode=1;
  glass.paintClip=RoundedPaintClip{0,0,30,30,10};control->setStyle(glass);
  viewport->addChild(std::move(control));root.addChild(std::move(viewport));
  Node::setDefaultCornerPower(4);
  CHECK(controlPtr->cornerPower()==4);
  CHECK(root.paintDirty());
  auto scene=collectMaterialScene(&root,200,120);
  CHECK(scene.planes.size()==2);
  CHECK(scene.planes[0].clip.width==200 && scene.planes[0].clip.x==0);
  CHECK(scene.planes[1].clip.x==50 && scene.planes[1].clip.y==40);
  CHECK(scene.planes[1].clip.width==100 && scene.planes[1].clip.height==40);
  CHECK(scene.planes[1].paintClip && scene.planes[1].paintClip->radius==10);
  CHECK(scene.planes[1].cornerPower==4 && scene.planes[1].paintClip->cornerPower==4);
  glass.paintClip->cornerPower=10;controlPtr->setStyle(glass);
  controlPtr->setCornerPower(2);
  scene=collectMaterialScene(&root,200,120);
  CHECK(scene.planes[1].cornerPower==2 && scene.planes[1].paintClip->cornerPower==10);
  CHECK(controlPtr->width()==30 && controlPtr->height()==30);
  CHECK(controlPtr->containsScenePoint(40.1F,30.1F)); // Rectangle hitbox survives a curved paint mask.
  // Popup transitions keep their final clip/input footprint and fade the
  // entire paint subtree. Exported material planes must inherit that opacity
  // and clip so compositor glass never appears behind still-fading content.
  Node popupRoot; popupRoot.setFrameSize(120, 60);
  auto popupPaint = std::make_unique<Node>(); auto* popupPaintPtr = popupPaint.get();
  popupPaint->setFrameSize(100, 40); popupPaint->setPosition(10, 10);
  popupPaint->setClipChildren(true); popupPaint->setOpacity(.4F);
  auto popupGlass = std::make_unique<RectNode>();
  popupGlass->setFrameSize(140, 40); popupGlass->setPosition(-20, 0);
  popupGlass->setStyle(glass); popupPaint->addChild(std::move(popupGlass));
  popupRoot.addChild(std::move(popupPaint));
  const auto popupScene = collectMaterialScene(&popupRoot, 120, 60);
  CHECK(popupScene.planes.size()==1 && popupScene.planes[0].opacity==.4F);
  CHECK(popupScene.planes[0].clip.x==10 && popupScene.planes[0].clip.y==10);
  CHECK(popupScene.planes[0].clip.width==100 && popupScene.planes[0].clip.height==40);
  Node segmentRoot;
  segmentRoot.setFrameSize(80, 24);
  auto segmentPlane = std::make_unique<RectNode>();
  segmentPlane->setFrameSize(80, 24);
  glass.paintClip.reset();
  glass.segmentContour = {.kind = SegmentContourKind::Powerline, .depth = 8.0F};
  segmentPlane->setStyle(glass);
  auto segmentChild = std::make_unique<RectNode>();
  segmentChild->setFrameSize(24, 16);
  segmentChild->setPosition(20, 4);
  auto childGlass = glass;
  childGlass.segmentContour = {};
  childGlass.materialPlane = false;
  childGlass.material->optical.planeMode = 0.0F;
  segmentChild->setStyle(childGlass);
  segmentPlane->addChild(std::move(segmentChild));
  segmentRoot.addChild(std::move(segmentPlane));
  // The rounded-only external codec omits the contoured local parent, but it
  // must still suppress inherited rounded descendants as one owned plane.
  CHECK(collectMaterialScene(&segmentRoot, 80, 24).planes.empty());
  glass.segmentContour = {};
  ImageNode image; image.setCornerPower(2); image.setRadius(15); image.setFrameSize(30,30);
  Node::setDefaultCornerPower(10);CHECK(image.cornerPower()==2 && image.radius()==15);
  // An invalidation callback may destroy a different registered root.
  auto first=std::make_unique<Node>(); auto second=std::make_unique<Node>();
  first->setInvalidationCallback([&](NodeInvalidation){second.reset();});
  second->setInvalidationCallback([&](NodeInvalidation){first.reset();});
  Node::setDefaultCornerPower(4);CHECK(!first || !second);
  first.reset();second.reset();
  Node::setDefaultCornerPower(2);
  glass.material->optical.planeMode=0;controlPtr->setStyle(glass);
  CHECK(collectMaterialScene(&root,200,120).planes.size()==1);
  root.setMaterialBackdropLocal(true);
  CHECK(controlPtr->materialBackdropLocal());
  CHECK(collectMaterialScene(&root,200,120).planes.empty());
  // A borrowed opener has no opacity on its original bar, but its proxy owns
  // the fade and relocates independent optical controls into the panel scene.
  Node panelRoot; panelRoot.setFrameSize(300,200);
  auto opener=std::make_unique<Node>(); opener->setPosition(90,80); opener->setOpacity(0);
  auto optical=std::make_unique<RectNode>(); optical->setPosition(3,4); optical->setFrameSize(20,10);
  glass.paintClip.reset(); glass.material->optical.planeMode=1; optical->setStyle(glass);
  opener->addChild(std::move(optical));
  auto borrowed=std::make_unique<RenderProxyNode>();auto* borrowedPtr=borrowed.get();
  borrowed->setPosition(40,50);borrowed->setFrameSize(30,20);borrowed->setOpacity(.25F);
  borrowed->setSourceProvider([&]() -> const Node* { return opener.get(); });
  panelRoot.addChild(std::move(borrowed));
  auto borrowedScene=collectMaterialScene(&panelRoot,300,200);
  CHECK(borrowedScene.planes.size()==1);
  CHECK(borrowedScene.planes[0].transform[4]==43 && borrowedScene.planes[0].transform[5]==54);
  CHECK(borrowedScene.planes[0].opacity==.25F);
  opener.reset();CHECK(collectMaterialScene(&panelRoot,300,200).planes.empty());
  opener=std::make_unique<Node>();
  auto replacement=std::make_unique<RectNode>();replacement->setFrameSize(12,13);replacement->setStyle(glass);
  opener->addChild(std::move(replacement));
  CHECK(collectMaterialScene(&panelRoot,300,200).planes.size()==1);
  borrowedPtr->setOpacity(0);CHECK(collectMaterialScene(&panelRoot,300,200).planes.empty());
  borrowedPtr->setOpacity(1);opener->setMaterialBackdropLocal(true);
  CHECK(collectMaterialScene(&panelRoot,300,200).planes.empty());

  // A custom background is an independent scene target even when its native
  // fallback primitive is flat. Parameters and source-derived transport identity
  // remain attached to that one background plane; children stay client-rendered.
  Node customRoot; customRoot.setFrameSize(90, 30);
  auto customRect = std::make_unique<RectNode>();
  customRect->setFrameSize(90, 30);
  RoundedRectStyle customStyle;
  customStyle.fill = rgba(1, 1, 1);
  auto customAsset = std::make_shared<CustomEffectAsset>(CustomEffectAsset{
      .stableId = "user.scope-test", .sha256Digest = std::string(64, 'a'),
      .source = "vec4 noctalia_effect(vec4 s,vec4 b,vec2 u,vec2 p,vec2 z,vec4 p0,vec4 p1,vec4 p2,vec4 p3,vec4 p4,vec4 p5,vec4 p6,vec4 p7){return s;}",
  });
  customStyle.customBackground = CustomEffectBinding{.asset = customAsset};
  customStyle.customBackground->parameters[0][0] = 3.0F;
  customRect->setStyle(customStyle);
  customRoot.addChild(std::move(customRect));
  const auto customScene = collectMaterialScene(&customRoot, 90, 30);
  CHECK(customScene.planes.size() == 1 && customScene.planes[0].customEffect);
  CHECK(customScene.planes[0].parameters.primitive == Primitive::Flat);
  CHECK(customScene.planes[0].customEffect->parameters[0][0] == 3.0F);

  // Explicit nested identity paths own independent optical planes. They must
  // survive containment by the bar plane so same-type sibling widgets can use
  // different native material parameters.
  Style::MaterialOverrides widgetOverrides;
  widgetOverrides.surfaces["bar.widget.meter-a"].primitive = Primitive::Optical;
  widgetOverrides.surfaces["bar.widget.meter-b"].primitive = Primitive::Optical;
  widgetOverrides.surfaces["bar.widget.meter-a"].tint_opacity = 0.0F;
  widgetOverrides.surfaces["bar.widget.meter-a"].rim = 0.2F;
  widgetOverrides.surfaces["bar.widget.meter-b"].rim = 0.8F;
  Style::setMaterialOverrides(widgetOverrides);
  Node widgetRoot; widgetRoot.setFrameSize(160, 36);
  auto barPlane = std::make_unique<RectNode>();
  barPlane->setFrameSize(160, 36);
  RoundedRectStyle barGlass; barGlass.fill = rgba(1, 1, 1);
  barGlass.material = Parameters{};
  barGlass.material->primitive = Primitive::Optical;
  barGlass.materialPlane = true;
  barPlane->setStyle(barGlass);
  for (const auto& target : {std::string{"bar.widget.meter-a"}, std::string{"bar.widget.meter-b"}}) {
    auto widgetPlane = std::make_unique<RectNode>();
    auto* widgetPlanePtr = widgetPlane.get();
    widgetPlane->setFrameSize(52, 24);
    widgetPlane->setPosition(target.ends_with('a') ? 8.0F : 72.0F, 6.0F);
    RoundedRectStyle carrier; carrier.fill = rgba(1, 1, 1); carrier.radius = Radii{10};
    SurfaceMaterial material;
    Node owner;
    material.syncPath(owner, *widgetPlanePtr, carrier, 0.7F, "surface", "bar-widget",
                      {"bar", "bar.instance.default", "bar.section.default.status", target});
    CHECK(widgetPlanePtr->style().material->optical.planeMode == 1.0F);
    CHECK(widgetPlanePtr->width() == 52 && widgetPlanePtr->height() == 24);
    barPlane->addChild(std::move(widgetPlane));
  }
  widgetRoot.addChild(std::move(barPlane));
  const auto widgetScene = collectMaterialScene(&widgetRoot, 160, 36);
  CHECK(widgetScene.planes.size() == 3);
  CHECK(widgetScene.planes[1].parameters.optical.tintOpacity == 0.0F);
  CHECK(widgetScene.planes[1].parameters.optical.rim != widgetScene.planes[2].parameters.optical.rim);
  RenderProxyNode cycleA,cycleB;
  cycleA.setSource(&cycleB);cycleB.setSource(&cycleA);
  CHECK(collectMaterialScene(&cycleA,300,200).planes.empty());
  std::puts("actual scene attachment, reparenting and material scope checks passed");
}
