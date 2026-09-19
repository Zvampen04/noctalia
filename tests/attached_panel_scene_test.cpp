#include "shell/panel/panel_manager.h"
#include "shell/panel/panel.h"
#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "render/core/renderer.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "ui/style.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "tests/test_check.h"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
class MeasurementRenderer final : public Renderer {
public:
  TextMetrics measureText(std::string_view text, float size, FontWeight, float maxWidth, int,
      TextAlign, std::string_view, TextEllipsize, bool) override {
    const float width = static_cast<float>(text.size()) * size * .5F;
    return {.width = maxWidth > 0 ? std::min(width,maxWidth) : width, .right = width, .bottom = size, .lineCount = 1};
  }
  TextMetrics measureFont(float size, FontWeight) override { return {.bottom = size}; }
  void measureTextCursorStops(std::string_view, float size, const std::vector<std::size_t>& offsets,
      std::vector<float>& result, FontWeight) override {
    result.clear(); for (auto offset : offsets) result.push_back(static_cast<float>(offset) * size * .5F);
  }
  void measureTextCursorStopsWrapped(std::string_view, float, const std::vector<std::size_t>&,
      float, std::vector<TextCursorStop>&, FontWeight) override {}
  TextMetrics measureGlyph(char32_t, float size) override { return {.width = size, .right = size, .bottom = size}; }
  TextureManager& textureManager() override { std::abort(); }
  float renderScale() const noexcept override { return 1; }
};
class RetainedPanel final : public Panel {
public:
  Input* input = nullptr;
  PanelPlacement placement = PanelPlacement::Attached;
  bool fillWidth = false;
  int creates = 0, updates = 0, layouts = 0;
  float preferredW = 400, preferredH = 260, laidOutW = 0, laidOutH = 0;
  void create() override {
    ++creates;
    auto box = std::make_unique<Flex>();
    box->setDirection(FlexDirection::Vertical);
    auto field = std::make_unique<Input>(); input = field.get();
    input->setValue("keep my query"); box->addChild(std::move(field));
    setRoot(std::move(box));
  }
  float preferredWidth() const override { return preferredW; }
  float preferredHeight() const override { return preferredH; }
  PanelPlacement panelPlacement() const noexcept override { return placement; }
  bool fillsWidth() const noexcept override { return fillWidth; }
  void doUpdate(Renderer&) override { ++updates; }
  void doLayout(Renderer& renderer, float width, float height) override {
    ++layouts; laidOutW = width; laidOutH = height;
    root()->setSize(width,height); root()->layout(renderer);
  }
};
class PrivateFiles {
  std::filesystem::path root;
  std::vector<std::pair<std::string,std::optional<std::string>>> saved;
public:
  PrivateFiles() {
    std::string pattern = (std::filesystem::temp_directory_path()/"noctalia-panel-scene-XXXXXX").string();
    TEST_CHECK(mkdtemp(pattern.data())); root = pattern;
    for (const auto* key : {"NOCTALIA_CONFIG_HOME","NOCTALIA_STATE_HOME","NOCTALIA_DATA_HOME","XDG_CONFIG_HOME","XDG_STATE_HOME","XDG_DATA_HOME","XDG_CACHE_HOME","XDG_RUNTIME_DIR"}) {
      const char* prior = std::getenv(key); saved.emplace_back(key,prior ? std::optional<std::string>(prior) : std::nullopt);
      const auto path = root/key; std::filesystem::create_directories(path/"noctalia");
      TEST_CHECK(setenv(key,path.c_str(),1) == 0);
    }
  }
  ~PrivateFiles() {
    for (const auto& [key,value] : saved) { if(value) setenv(key.c_str(),value->c_str(),1); else unsetenv(key.c_str()); }
    std::filesystem::remove_all(root);
  }
};
bool near(float a,float b) { return std::abs(a-b)<.01F; }
}

class WaylandConnectionTestAccess {
public:
  static wl_output* output(WaylandConnection& connection) {
    auto* pointer = reinterpret_cast<wl_output*>(std::uintptr_t{0x1234});
    connection.m_outputs.push_back({.connectorName="fixture",.output=pointer,.logicalWidth=1000,.logicalHeight=700});
    return pointer;
  }
  static void clear(WaylandConnection& connection) { connection.m_outputs.clear(); }
};

class PanelManagerLayoutTestAccess {
public:
  static void run(PanelManager& manager, WaylandConnection& wayland, CompositorPlatform& platform,
                  ConfigService& config, MeasurementRenderer& renderer) {
    manager.m_platform = &platform; manager.m_config = &config;
    manager.m_output = WaylandConnectionTestAccess::output(wayland);
    manager.m_surface = std::make_unique<LayerSurface>(wayland,LayerSurfaceConfig{});
    manager.m_layerSurface = static_cast<LayerSurface*>(manager.m_surface.get());
    auto panel = std::make_unique<RetainedPanel>(); auto* content = panel.get(); panel->create();
    manager.m_activePanel = content; manager.m_activePanelId = "launcher";
    manager.m_panels.emplace("launcher",std::move(panel));
    manager.m_sceneRoot = std::make_unique<Node>();
    auto reveal = std::make_unique<Node>(); reveal->setClipChildren(true);
    manager.m_attachedRevealClipNode = manager.m_sceneRoot->addChild(std::move(reveal));
    manager.m_attachedRevealContentNode = manager.m_attachedRevealClipNode->addChild(std::make_unique<Node>());
    auto bg = std::make_unique<Box>(); manager.m_bgNode = manager.m_attachedRevealContentNode->addChild(std::move(bg));
    auto clip = std::make_unique<Node>(); clip->setClipChildren(true);
    manager.m_attachedContentClipNode = manager.m_attachedRevealContentNode->addChild(std::move(clip));
    manager.m_contentNode = manager.m_attachedContentClipNode->addChild(std::make_unique<Node>());
    manager.m_contentNode->addChild(content->releaseRoot());
    manager.m_inputDispatcher.setSceneRoot(manager.m_sceneRoot.get());
    manager.m_inputDispatcher.setFocus(content->input->inputArea());
    manager.m_attachedToBar = true; manager.m_attachedRevealProgress = 1;
    manager.m_sourceBarName = manager.m_openingSourceBarName = "first";
    manager.m_attachedAnchorAvailable = true; manager.m_attachedAnchorX = 800; manager.m_attachedAnchorY = 500;
    manager.m_panelOutputInputRect = InputRect{};
    auto* scene = manager.m_sceneRoot.get(); auto* input = content->input;
    auto& cfg = const_cast<Config&>(config.config());
    cfg.bars.clear();
    BarConfig first; first.name="first"; first.position="top"; first.thickness=40; first.reserveSpace=false;
    first.marginEnds=0; first.marginEdge=0;
    auto second=first; second.name="second"; second.position="left"; second.layer="overlay";
    cfg.bars={first,second}; cfg.shell.panel.launcherPlacement=PanelPlacement::Attached;
    cfg.shell.panel.openNearClickLauncher=false;
    std::vector<std::string> cleared;
    manager.setAttachedPanelGeometryCallback([&](wl_output*,std::string_view name,auto geometry) {
      if (!geometry) cleared.emplace_back(name);
    });
    manager.refreshPanelPlacement();
    TEST_CHECK(manager.m_attachedAwaitingConfigure && manager.m_attachedPlacementPending);
    const auto pending=*manager.m_attachedPlacement;
    manager.applyPanelPlacement(pending.surfaceWidth,pending.surfaceHeight);
    TEST_CHECK(manager.m_panelVisualWidth==0); // no scene mutation before accepted configure
    manager.onSurfaceConfigured();
    manager.applyPanelPlacement(pending.surfaceWidth,pending.surfaceHeight);
    manager.layoutScene(renderer,pending.surfaceWidth,pending.surfaceHeight);
    TEST_CHECK(content->creates==1 && input->value()=="keep my query");
    TEST_CHECK(manager.m_inputDispatcher.focusedArea()==input->inputArea());
    const int centered=manager.m_attachedPlacement->body.x;
    cfg.shell.panel.openNearClickLauncher=true;
    manager.refreshPanelPlacement();
    TEST_CHECK(manager.m_attachedHasAnchor && manager.m_attachedPlacement->body.x>centered);
    manager.onSurfaceConfigured();
    manager.applyPanelPlacement(pending.surfaceWidth,pending.surfaceHeight);
    manager.layoutScene(renderer,pending.surfaceWidth,pending.surfaceHeight);

    // Switch the retained surface's owner and edge; no second create/focus reset.
    cfg.shell.panelAnchorBar="second";
    manager.refreshPanelPlacement();
    const auto moved=*manager.m_attachedPlacement;
    manager.onSurfaceConfigured();
    manager.applyPanelPlacement(moved.surfaceWidth-90,moved.surfaceHeight-60);
    manager.layoutScene(renderer,moved.surfaceWidth-90,moved.surfaceHeight-60);
    TEST_CHECK(manager.m_sourceBarName=="second" && manager.m_panelLayer==LayerShellLayer::Overlay);
    TEST_CHECK(!cleared.empty() && cleared.back()=="first");
    TEST_CHECK(static_cast<Box*>(manager.m_bgNode)->style().corners==attached_panel::cornerShapes("left"));
    TEST_CHECK(manager.m_panelInsetX+manager.m_panelVisualWidth+manager.m_attachedBleedRight<=moved.surfaceWidth-90);
    TEST_CHECK(manager.m_panelInsetY+manager.m_panelVisualHeight+manager.m_attachedBleedBottom<=moved.surfaceHeight-60);
    TEST_CHECK(near(content->laidOutW,manager.m_panelVisualWidth-2*Style::panelPadding));
    TEST_CHECK(manager.m_sceneRoot.get()==scene && content->input==input && content->creates==1);
    TEST_CHECK(input->value()=="keep my query" && manager.m_inputDispatcher.focusedArea()==input->inputArea());

    // Changing padding at an identical buffer extent must invalidate the retained
    // host layout, not just re-layout the panel at its old cached content size.
    const float padding=Style::panelPadding;
    const float oldContentWidth=content->laidOutW;
    auto metrics=Style::metrics(); metrics.panelPadding=padding+7; Style::setMetrics(metrics);
    manager.onConfigReloaded();
    TEST_CHECK(manager.m_sceneGeometryDirty);
    manager.onSurfaceConfigured();
    manager.applyPanelPlacement(moved.surfaceWidth-90,moved.surfaceHeight-60);
    manager.layoutScene(renderer,moved.surfaceWidth-90,moved.surfaceHeight-60);
    TEST_CHECK(near(content->laidOutW,oldContentWidth-14));
    TEST_CHECK(!manager.m_sceneGeometryDirty && manager.m_inputDispatcher.focusedArea()==input->inputArea());
    metrics.panelPadding=padding; Style::setMetrics(metrics);

    // Legacy slide must refresh the same background radii/insets/corner orientation.
    cfg.shell.panel.attachedMorph=false; cfg.bars[1].position="bottom";
    manager.refreshPanelPlacement(); const auto bottom=*manager.m_attachedPlacement;
    manager.onSurfaceConfigured(); manager.applyPanelPlacement(bottom.surfaceWidth,bottom.surfaceHeight);
    manager.layoutScene(renderer,bottom.surfaceWidth,bottom.surfaceHeight);
    TEST_CHECK(static_cast<Box*>(manager.m_bgNode)->style().corners==attached_panel::cornerShapes("bottom"));
    TEST_CHECK(static_cast<Box*>(manager.m_bgNode)->style().logicalInset==attached_panel::logicalInset("bottom",Style::scaledRadiusXl(1)));
    TEST_CHECK(manager.m_sceneRoot.get()==scene && content->creates==1 && input->value()=="keep my query");
    // A bar callback can cause another configuration change. Do not consume the
    // newer pending placement while finishing the older source-bar transition.
    cfg.shell.panelAnchorBar="first";
    manager.refreshPanelPlacement(); const auto stale=*manager.m_attachedPlacement;
    manager.onSurfaceConfigured();
    bool redirected=false;
    manager.setAttachedPanelGeometryCallback([&](wl_output*,std::string_view name,auto geometry) {
      if (!redirected && !geometry && name=="second") {
        redirected=true; cfg.shell.panelAnchorBar="second"; manager.refreshPanelPlacement();
      }
    });
    manager.applyPanelPlacement(stale.surfaceWidth,stale.surfaceHeight);
    TEST_CHECK(redirected && manager.m_attachedPlacementPending && manager.m_sourceBarName=="second");
    const auto newest=*manager.m_attachedPlacement;
    manager.onSurfaceConfigured(); manager.applyPanelPlacement(newest.surfaceWidth,newest.surfaceHeight);
    manager.layoutScene(renderer,newest.surfaceWidth,newest.surfaceHeight);
    TEST_CHECK(!manager.m_attachedPlacementPending && manager.m_sourceBarName=="second");
    TEST_CHECK(manager.m_inputDispatcher.focusedArea()==input->inputArea() && input->value()=="keep my query");
    // Placement changes reuse the actual scene/surface and typed, focused Input.
    manager.setAttachedPanelGeometryCallback([&](wl_output*,std::string_view name,auto geometry) {
      if (!geometry) cleared.emplace_back(name);
    });
    auto* const surface = manager.m_surface.get();
    const auto applyPending = [&] {
      const auto next = *manager.m_attachedPlacement;
      manager.onSurfaceConfigured();
      manager.applyPanelPlacement(next.surfaceWidth,next.surfaceHeight);
      manager.layoutScene(renderer,next.surfaceWidth,next.surfaceHeight);
      TEST_CHECK(manager.m_surface.get()==surface && manager.m_sceneRoot.get()==scene);
      TEST_CHECK(content->creates==1 && content->input==input && input->value()=="keep my query");
      TEST_CHECK(manager.m_inputDispatcher.focusedArea()==input->inputArea());
      TEST_CHECK(manager.m_panelOutputInputRect->width==static_cast<int>(manager.m_panelVisualWidth));
    };
    content->placement=PanelPlacement::Floating;
    cfg.shell.panel.launcherPlacement=PanelPlacement::Floating;
    cfg.shell.panel.launcherPosition="top_right";
    manager.onConfigReloaded();
    TEST_CHECK(!manager.m_attachedPlacement->attached && manager.m_attachedPlacement->anchoredRight);
    applyPending();
    TEST_CHECK(!manager.isAttachedOpen() && !manager.m_attachedPanelGeometry);
    TEST_CHECK(cleared.back()=="second" && manager.m_attachedBarPosition.empty());
    TEST_CHECK(manager.m_detachedRevealClipNode && !manager.m_attachedRevealClipNode);
    TEST_CHECK(!manager.m_attachedContentClipNode->clipChildren());
    TEST_CHECK(static_cast<Box*>(manager.m_bgNode)->style().corners==CornerShapes{});
    TEST_CHECK(static_cast<Box*>(manager.m_bgNode)->style().logicalInset==RectInsets{});
    TEST_CHECK(near(manager.m_detachedRevealProgress,1));

    // Simulate the same wrapper topology as a panel first opened detached.
    // Reattaching must add its clip without destroying/recreating the Input.
    auto retained=manager.m_attachedContentClipNode->removeChild(manager.m_contentNode);
    auto retiredClip=manager.m_detachedRevealContentNode->removeChild(manager.m_attachedContentClipNode);
    manager.m_attachedContentClipNode=nullptr;
    manager.m_detachedRevealContentNode->addChild(std::move(retained));
    retiredClip.reset();
    content->placement=PanelPlacement::Attached;
    cfg.shell.panel.launcherPlacement=PanelPlacement::Attached;
    manager.onConfigReloaded();applyPending();
    TEST_CHECK(manager.isAttachedOpen() && manager.m_attachedContentClipNode->clipChildren());
    TEST_CHECK(static_cast<Box*>(manager.m_bgNode)->style().corners==attached_panel::cornerShapes("bottom"));

    // Disabling or removing the named source centers the same open panel; it
    // cannot keep publishing a join to a bar which is no longer available.
    cfg.bars[1].enabled=false;
    manager.onConfigReloaded();applyPending();
    TEST_CHECK(!manager.isAttachedOpen() && manager.m_sourceBarName.empty());
    TEST_CHECK(!manager.m_attachedPanelGeometry && cleared.back()=="second");
    TEST_CHECK(manager.m_panelOutputInputRect->x==(1000-static_cast<int>(manager.m_panelVisualWidth))/2);
    TEST_CHECK(manager.m_panelOutputInputRect->y==(700-static_cast<int>(manager.m_panelVisualHeight))/2);
    cfg.bars[1].enabled=true;
    manager.onConfigReloaded();applyPending();
    TEST_CHECK(manager.isAttachedOpen() && manager.m_sourceBarName=="second");
    const auto restored=cfg.bars[1];cfg.bars.erase(cfg.bars.begin()+1);
    manager.onConfigReloaded();applyPending();
    TEST_CHECK(!manager.isAttachedOpen() && manager.m_sourceBarName.empty());
    cfg.bars.push_back(restored);
    manager.onConfigReloaded();applyPending();
    TEST_CHECK(manager.isAttachedOpen() && manager.m_sourceBarName=="second");

    // One island silhouette owns the opener, without rebuilding focused content.
    {
      cfg.bars[1].sectionBackgrounds=true;cfg.bars[1].islandMorph=true;
      manager.m_attachedSource={.section=AttachedPanelSourceSection::Center,
          .x=700,.y=660,.width=80,.height=32,.radii={4,8,12,16}};
      std::optional<AttachedPanelSource> currentSource=manager.m_attachedSource;
      manager.setAttachedSourceGeometryProvider([&](wl_output*,std::string_view,const AttachedPanelSource&) {
        return currentSource;
      });
      auto opener=std::make_unique<Node>();opener->setFrameSize(60,20);opener->setOpacity(0);
      manager.setAttachedSourceContentProvider([&](wl_output*,std::string_view,const AttachedPanelSource&) {
        return opener.get();
      });
      std::optional<AttachedPanelGeometry> published;
      manager.setAttachedPanelGeometryCallback([&](wl_output*,std::string_view,auto geometry){published=geometry;});
      manager.onConfigReloaded();applyPending();
      TEST_CHECK(manager.m_islandMorph && manager.m_attachedPlacement->islandMorph);
      manager.applyAttachedReveal(.25F);
      TEST_CHECK(published && published->panelOwnsSource && published->bulgeRadius==0);
      TEST_CHECK(manager.m_islandOpenerProxy && manager.m_islandOpenerProxy->source()==opener.get());
      TEST_CHECK(near(manager.m_islandOpenerProxy->opacity(),.75F));
      TEST_CHECK(near(manager.m_contentNode->opacity(),.25F));
      TEST_CHECK(static_cast<Box*>(manager.m_bgNode)->style().corners==CornerShapes{});
      TEST_CHECK(static_cast<Box*>(manager.m_bgNode)->style().logicalInset==RectInsets{});
      TEST_CHECK(!manager.m_islandOpenerProxy->hitTestVisible());
      // Clipped end/start lanes keep their pre-reflow origin, not the center of
      // the remaining visible island. Exercise horizontal and vertical clipping.
      for (const auto offset : {AttachedPanelSource::ContentOffset{-35,6},
                                AttachedPanelSource::ContentOffset{4,-27}}) {
        manager.m_attachedSource.contentOffset=offset;
        manager.applyAttachedReveal(.25F);
        TEST_CHECK(near(manager.m_islandOpenerProxy->x(), manager.m_attachedSource.x
            - manager.m_attachedPanelGeometry->finalOutputX + manager.m_panelInsetX + offset.x));
        TEST_CHECK(near(manager.m_islandOpenerProxy->y(), manager.m_attachedSource.y
            - manager.m_attachedPanelGeometry->finalOutputY + manager.m_panelInsetY + offset.y));
      }
      manager.m_attachedSource.contentOffset.reset();
      // A replacement bar subtree must never leave the proxy with its old pointer.
      opener.reset();TEST_CHECK(manager.m_islandOpenerProxy->source()==nullptr);
      opener=std::make_unique<Node>();opener->setFrameSize(64,20);
      manager.applyAttachedReveal(.75F);
      TEST_CHECK(manager.m_islandOpenerProxy->source()==opener.get());
      TEST_CHECK(near(manager.m_islandOpenerProxy->opacity(),.25F));
      manager.applyAttachedReveal(1.F);
      TEST_CHECK(!manager.m_islandOpenerProxy->visible());
      manager.applyAttachedReveal(.25F);
      TEST_CHECK(manager.m_islandOpenerProxy->visible());
      // Theme/output relayout refreshes the compact reverse-animation target.
      currentSource->x=740;currentSource->width=100;currentSource->radii={8,10,12,14};
      manager.onConfigReloaded();applyPending();
      TEST_CHECK(manager.m_attachedSource.x==740 && manager.m_attachedSource.width==100);
      TEST_CHECK(manager.m_attachedSource.radii==currentSource->radii);
      currentSource.reset();manager.refreshPanelPlacement();applyPending();
      TEST_CHECK(manager.m_attachedSource.x==740); // temporarily unconfigured bar
      TEST_CHECK(input->value()=="keep my query" && manager.m_inputDispatcher.focusedArea()==input->inputArea());
      manager.setAttachedSourceContentProvider({});
      manager.setAttachedSourceGeometryProvider({});
      manager.setAttachedPanelGeometryCallback([&](wl_output*,std::string_view name,auto geometry) {
        if (!geometry) cleared.emplace_back(name);
      });
      cfg.bars[1].islandMorph=false;manager.m_attachedRevealProgress=1;
      manager.onConfigReloaded();applyPending();
    }

    // A later config reload must retain a compositor-constrained fill request,
    // rather than waiting indefinitely for an identical new configure.
    content->placement=PanelPlacement::Floating;content->fillWidth=true;
    cfg.shell.panel.launcherPlacement=PanelPlacement::Floating;
    manager.onConfigReloaded();applyPending();
    const auto filled=*manager.m_attachedPlacement;
    TEST_CHECK(filled.fillWidth && !manager.m_attachedAwaitingConfigure);
    manager.refreshPanelPlacement();
    TEST_CHECK(manager.m_attachedPlacement->request==filled.request && !manager.m_attachedAwaitingConfigure);
    applyPending();

    manager.m_attachedPanelGeometryCallback={}; manager.m_output=nullptr;
    WaylandConnectionTestAccess::clear(wayland);
  }
};
int main() {
  PrivateFiles files;
  ConfigService config;
  WaylandConnection wayland;
  CompositorPlatform platform(wayland);
  MeasurementRenderer renderer;
  PanelManager manager;
  PanelManagerLayoutTestAccess::run(manager,wayland,platform,config,renderer);
}
