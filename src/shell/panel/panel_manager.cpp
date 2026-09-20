#include "shell/panel/panel_manager.h"
#include "render/scene/countdown_ring_node.h"
#include "shell/panel/attached_panel_layout.h"
#include "core/input/key_symbols.h"
#include "ui/controls/slider.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/input/key_chord.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/animation/motion_service.h"
#include "render/scene/input_area.h"
#include "scripting/plugin_id.h"
#include "shell/bar/bar_corner_shape.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_surface_style.h"
#include "shell/screen_position.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/context_menu_popup.h"
#include "ui/controls/select_dropdown_popup.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "util/sys_utils.h"
#include "wayland/hyprland/focus_grab_service.h"
#include "wayland/layer_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>

PanelManager* PanelManager::s_instance = nullptr;

namespace {

  constexpr Logger kLog("panel");
  constexpr auto kKeyboardRelaxDelay = std::chrono::milliseconds(100);

  // Layer-shell keyboard interactivity for a panel surface: `initial` is applied at
  // map time, `relaxed` (when set) shortly after by m_keyboardRelaxTimer.
  struct PanelKeyboardPlan {
    LayerShellKeyboard initial = LayerShellKeyboard::None;
    std::optional<LayerShellKeyboard> relaxed;
  };

  // A panel asking for None never takes keyboard focus, so the app the user is
  // typing into keeps it. Everything else reproduces the per-path behavior the
  // compositor workarounds need: a focus grab maps Exclusive so the panel wins the
  // keyboard from the grab and hands it back once settled, and an attached panel
  // takes focus only after the bar-anchored map, which the reveal would otherwise
  // race. `mode` therefore only selects between OnDemand and Exclusive on the
  // detached path of a compositor without focus-grab support.
  PanelKeyboardPlan
  resolvePanelKeyboardPlan(LayerShellKeyboard mode, bool hasFocusGrab, bool grabWillActivate, bool attached) {
    if (mode == LayerShellKeyboard::None) {
      return {.initial = LayerShellKeyboard::None, .relaxed = std::nullopt};
    }
    if (hasFocusGrab) {
      return {
          .initial = LayerShellKeyboard::Exclusive,
          .relaxed = grabWillActivate ? std::optional{LayerShellKeyboard::OnDemand} : std::nullopt
      };
    }
    if (attached) {
      return {.initial = LayerShellKeyboard::None, .relaxed = LayerShellKeyboard::Exclusive};
    }
    return {.initial = mode, .relaxed = std::nullopt};
  }

  bool blurTraceEnabled() {
    static const bool enabled = SysUtils::isEnvFlagOn("NOCTALIA_BLUR_TRACE");
    return enabled;
  }

  struct BarVisibleRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
  };

  BarVisibleRect
  resolveBarVisibleRect(const BarConfig& barConfig, std::int32_t outputWidth, std::int32_t outputHeight) {
    const bool barIsBottom = barConfig.position == "bottom";
    const bool barIsLeft = barConfig.position == "left";
    const bool barIsRight = barConfig.position == "right";
    const bool barIsVertical = barIsLeft || barIsRight;
    const std::int32_t mEdge = std::max(0, barConfig.marginEdge);
    const std::int32_t mEnds = std::max(0, barConfig.marginEnds);
    const std::int32_t thickness = std::max(0, barConfig.thickness);

    const std::int32_t left =
        barIsRight ? std::max(0, outputWidth - mEdge - thickness) : (barIsVertical ? mEdge : mEnds);
    const std::int32_t top =
        barIsBottom ? std::max(0, outputHeight - mEdge - thickness) : (barIsVertical ? mEnds : mEdge);
    const std::int32_t right = barIsVertical ? left + thickness : std::max(left, outputWidth - mEnds);
    const std::int32_t bottom = barIsVertical ? std::max(top, outputHeight - mEnds) : top + thickness;

    return BarVisibleRect{
        .left = left,
        .top = top,
        .right = right,
        .bottom = bottom,
    };
  }

  std::int32_t anchoredSurfaceOrigin(
      std::uint32_t anchor, std::uint32_t startAnchor, std::uint32_t endAnchor, std::int32_t startMargin,
      std::int32_t endMargin, std::int32_t outputExtent, std::int32_t surfaceExtent
  ) {
    const bool anchoredStart = (anchor & startAnchor) != 0;
    const bool anchoredEnd = (anchor & endAnchor) != 0;
    if (anchoredStart != anchoredEnd) {
      return anchoredStart ? startMargin : outputExtent - surfaceExtent - endMargin;
    }
    return (outputExtent - surfaceExtent) / 2;
  }

  InputRect panelInputRectForSurface(
      std::uint32_t anchor, std::int32_t marginTop, std::int32_t marginRight, std::int32_t marginBottom,
      std::int32_t marginLeft, std::int32_t outputWidth, std::int32_t outputHeight, std::uint32_t surfaceWidth,
      std::uint32_t surfaceHeight, std::int32_t insetX, std::int32_t insetY, std::uint32_t panelWidth,
      std::uint32_t panelHeight
  ) {
    const auto resolvedSurfaceWidth = static_cast<std::int32_t>(surfaceWidth);
    const auto resolvedSurfaceHeight = static_cast<std::int32_t>(surfaceHeight);
    const auto surfaceX = anchoredSurfaceOrigin(
        anchor, LayerShellAnchor::Left, LayerShellAnchor::Right, marginLeft, marginRight, outputWidth,
        resolvedSurfaceWidth
    );
    const auto surfaceY = anchoredSurfaceOrigin(
        anchor, LayerShellAnchor::Top, LayerShellAnchor::Bottom, marginTop, marginBottom, outputHeight,
        resolvedSurfaceHeight
    );
    return InputRect{surfaceX + insetX, surfaceY + insetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)};
  }

  InputRect boundsForPanelTrace(const std::vector<InputRect>& rects) {
    if (rects.empty()) {
      return {};
    }

    int minX = rects.front().x;
    int minY = rects.front().y;
    int maxX = rects.front().x + rects.front().width;
    int maxY = rects.front().y + rects.front().height;
    for (const auto& rect : rects) {
      minX = std::min(minX, rect.x);
      minY = std::min(minY, rect.y);
      maxX = std::max(maxX, rect.x + rect.width);
      maxY = std::max(maxY, rect.y + rect.height);
    }

    return InputRect{minX, minY, maxX - minX, maxY - minY};
  }

  // Resolves the bar a panel should attach to / position relative to.
  // `shell.panel_anchor_bar` wins when set; otherwise `barName` is the opening
  // source bar. A named bar that does not exist fails loudly (nullopt).
  // Prefer an enabled bar on the output; if none is enabled there (e.g. a bar-less
  // monitor), still return a resolved bar so openPanel can use a center-screen
  // floating layout via attached-panel availability.
  std::optional<BarConfig> resolvePanelBarConfig(
      ConfigService* configService, CompositorPlatform* platform, wl_output* output, std::string_view barName = {}
  ) {
    if (configService == nullptr || configService->config().bars.empty()) {
      return BarConfig{};
    }

    const auto& bars = configService->config().bars;
    const std::string_view panelAnchorBar = configService->config().shell.panelAnchorBar;
    const std::string_view effectiveName = !panelAnchorBar.empty() ? panelAnchorBar : barName;

    const WaylandOutput* wlOutput = nullptr;
    if (platform != nullptr && output != nullptr) {
      wlOutput = platform->findOutputByWl(output);
    }

    const auto resolve = [wlOutput](const BarConfig& bar) {
      return wlOutput != nullptr ? ConfigService::resolveForOutput(bar, *wlOutput) : bar;
    };

    if (!effectiveName.empty()) {
      for (const auto& bar : bars) {
        if (bar.name != effectiveName) {
          continue;
        }
        BarConfig resolved = resolve(bar);
        if (!resolved.enabled) {
          kLog.warn("panel: bar \"{}\" is disabled on this output; opening without bar attachment", effectiveName);
        }
        return resolved;
      }
      kLog.error("panel: bar \"{}\" not found (source bar or shell.panel_anchor_bar)", effectiveName);
      return std::nullopt;
    }

    for (const auto& bar : bars) {
      BarConfig resolved = resolve(bar);
      if (resolved.enabled) {
        return resolved;
      }
    }

    // Bar-less output: keep opening panels on floating surfaces rather than aborting.
    return resolve(bars.front());
  }

  bool hasMultipleEnabledBarsOnEdge(
      ConfigService* configService, CompositorPlatform* platform, wl_output* output, std::string_view position
  ) {
    if (configService == nullptr || position.empty()) {
      return false;
    }

    const WaylandOutput* wlOutput = nullptr;
    if (platform != nullptr && output != nullptr) {
      wlOutput = platform->findOutputByWl(output);
    }

    std::size_t count = 0;
    for (const auto& bar : configService->config().bars) {
      const BarConfig resolved = wlOutput != nullptr ? ConfigService::resolveForOutput(bar, *wlOutput) : bar;
      if (!resolved.enabled || resolved.position != position) {
        continue;
      }
      ++count;
      if (count > 1) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] float panelRevealContentOpacity(float reveal) {
    const float v = std::clamp(reveal, 0.0F, 1.0F);
    if (v <= 0.15F) {
      return 0.0F;
    }
    return std::clamp((v - 0.15F) / 0.85F, 0.0F, 1.0F);
  }

  [[nodiscard]] AttachedRevealDirection
  detachedRevealDirection(std::string_view panelPosition, std::string_view barPosition) {
    if (panelPosition == "top_left" || panelPosition == "top_center" || panelPosition == "top_right") {
      return AttachedRevealDirection::Down;
    }
    if (panelPosition == "bottom_left" || panelPosition == "bottom_center" || panelPosition == "bottom_right") {
      return AttachedRevealDirection::Up;
    }
    if (panelPosition == "center_left") {
      return AttachedRevealDirection::Right;
    }
    if (panelPosition == "center_right") {
      return AttachedRevealDirection::Left;
    }
    if (panelPosition == "center") {
      return AttachedRevealDirection::Down;
    }
    return attached_panel::revealDirection(barPosition);
  }

  // Floating screen position for a built-in panel (one of kPanelPositions).
  // "auto" = bar-relative (and the default for any non-built-in panel).
  [[nodiscard]] std::string resolvePanelPosition(const ConfigService* configService, std::string_view panelId) {
    if (configService == nullptr) {
      return "auto";
    }
    const auto& pc = configService->config().shell.panel;
    if (panelId == "control-center") {
      return pc.controlCenterPosition;
    }
    if (panelId == "launcher") {
      return pc.launcherPosition;
    }
    if (panelId == "clipboard") {
      return pc.clipboardPosition;
    }
    if (panelId == "wallpaper") {
      return pc.wallpaperPosition;
    }
    if (panelId == "session") {
      return pc.sessionPosition;
    }
    if (panelId == "polkit") {
      return pc.polkitPosition;
    }
    return "auto";
  }

  [[nodiscard]] bool usesConfiguredFloatingLayer(std::string_view panelId) {
    return panelId == "clipboard"
        || panelId == "control-center"
        || panelId == "launcher"
        || panelId == "session"
        || panelId == "setup-wizard"
        || panelId == "wallpaper";
  }

  [[nodiscard]] LayerShellLayer
  resolveFloatingPanelLayer(const ConfigService* configService, std::string_view panelId, const Panel& panel) {
    if (configService != nullptr && usesConfiguredFloatingLayer(panelId)) {
      return layerShellLayerFromConfig(configService->config().shell.panel.floatingLayer);
    }
    return panel.layer();
  }

  [[nodiscard]] bool openNearClickEnabledForPanel(const ConfigService* configService, std::string_view panelId) {
    if (panelId == "tray-drawer") {
      return true;
    }
    if (configService == nullptr) {
      return false;
    }
    const auto& pc = configService->config().shell.panel;
    // A floating panel pinned to a fixed screen position ignores open-near-click.
    const auto pinned = [](PanelPlacement placement, const std::string& position) {
      return placement == PanelPlacement::Floating && position != "auto";
    };
    if (panelId == "control-center") {
      return !pinned(pc.controlCenterPlacement, pc.controlCenterPosition) && pc.openNearClickControlCenter;
    }
    if (panelId == "launcher") {
      return !pinned(pc.launcherPlacement, pc.launcherPosition) && pc.openNearClickLauncher;
    }
    if (panelId == "clipboard") {
      return !pinned(pc.clipboardPlacement, pc.clipboardPosition) && pc.openNearClickClipboard;
    }
    if (panelId == "wallpaper") {
      return !pinned(pc.wallpaperPlacement, pc.wallpaperPosition) && pc.openNearClickWallpaper;
    }
    if (panelId == "session") {
      return !pinned(pc.sessionPlacement, pc.sessionPosition) && pc.openNearClickSession;
    }
    return false;
  }

  [[nodiscard]] bool
  openNearClickEnabled(const Panel* panel, std::string_view panelId, const ConfigService* configService) {
    if (panelId.contains(':')) {
      if (panel == nullptr) {
        return false;
      }
      const bool pinned = panel->panelPlacement() == PanelPlacement::Floating
          && panel->panelScreenPosition() != "auto"
          && panel->panelScreenPosition() != "center";
      return !pinned && panel->panelOpenNearClick();
    }
    return openNearClickEnabledForPanel(configService, panelId);
  }

} // namespace

PanelManager::PanelManager() { s_instance = this; }

PanelManager::~PanelManager() {
  m_persistentHost.setPanelClosedCallback(nullptr);
  if (s_instance == this) {
    s_instance = nullptr;
  }
}

PanelManager& PanelManager::instance() { return *s_instance; }

PanelManager* PanelManager::current() noexcept { return s_instance; }

WaylandConnection* PanelManager::wayland() const noexcept {
  return m_platform != nullptr ? &m_platform->wayland() : nullptr;
}

void PanelManager::initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext) {
  m_platform = &platform;
  m_config = config;
  m_renderContext = renderContext;
  m_clickShield.initialize(platform.wayland());
  m_persistentHost.initialize(platform, config, renderContext);
}

void PanelManager::setOpenSettingsWindowCallback(std::function<void(std::string)> callback) {
  m_openSettingsWindow = std::move(callback);
}

void PanelManager::setOpenWidgetSettingsCallback(std::function<void(std::string, std::string)> callback) {
  m_openWidgetSettings = std::move(callback);
}

void PanelManager::setOpenPluginSettingsCallback(std::function<bool(std::string)> callback) {
  m_openPluginSettings = std::move(callback);
}

void PanelManager::setCloseSettingsWindowCallback(std::function<void()> callback) {
  m_closeSettingsWindow = std::move(callback);
}

void PanelManager::setToggleSettingsWindowCallback(std::function<void(std::string)> callback) {
  m_toggleSettingsWindow = std::move(callback);
}

void PanelManager::setCloseDesktopWidgetsEditorCallback(std::function<void()> callback) {
  m_closeDesktopWidgetsEditor = std::move(callback);
}

void PanelManager::openSettingsWindow(std::string context) {
  if (isOpen() && !m_closing) {
    closePanel();
  }
  if (m_openSettingsWindow) {
    m_openSettingsWindow(std::move(context));
  }
}

bool PanelManager::openPluginSettings(const std::string& pluginId) {
  if (!m_openPluginSettings) {
    return false;
  }
  if (isOpen() && !m_closing) {
    closePanel();
  }
  return m_openPluginSettings(pluginId);
}

void PanelManager::closeSettingsWindow() {
  if (m_closeSettingsWindow) {
    m_closeSettingsWindow();
  }
}

void PanelManager::toggleSettingsWindow(std::string context) {
  if (isOpen() && !m_closing) {
    closePanel();
  }
  if (m_toggleSettingsWindow) {
    m_toggleSettingsWindow(std::move(context));
    return;
  }
  if (m_openSettingsWindow) {
    m_openSettingsWindow(std::move(context));
  }
}

void PanelManager::setAttachedPanelGeometryCallback(
    std::function<void(wl_output*, std::string_view, std::optional<AttachedPanelGeometry>)> callback
) {
  m_attachedPanelGeometryCallback = std::move(callback);
}

void PanelManager::setClickShieldExcludeRectsProvider(std::function<std::vector<InputRect>(wl_output*)> provider) {
  m_clickShieldExcludeRectsProvider = std::move(provider);
}

void PanelManager::setFocusGrabBarSurfacesProvider(std::function<std::vector<wl_surface*>()> provider) {
  m_focusGrabBarSurfacesProvider = std::move(provider);
}

void PanelManager::setPanelClosedCallback(std::function<void()> callback) {
  m_panelClosedCallback = std::move(callback);
  m_persistentHost.setPanelClosedCallback([this]() {
    if (m_panelClosedCallback) {
      m_panelClosedCallback();
    }
  });
}

void PanelManager::setPanelOpenedCallback(std::function<void()> callback) {
  m_panelOpenedCallback = std::move(callback);
}

void PanelManager::setAttachedPanelAvailabilityCallback(std::function<bool(wl_output*, std::string_view)> callback) {
  m_attachedPanelAvailabilityCallback = std::move(callback);
}

void PanelManager::setAttachedPanelLayerProvider(
    std::function<std::optional<std::string>(wl_output*, std::string_view)> provider
) {
  m_attachedPanelLayerProvider = std::move(provider);
}

void PanelManager::setAttachedPanelBarSettledCallback(std::function<bool(wl_output*, std::string_view)> callback) {
  m_attachedPanelBarSettledCallback = std::move(callback);
}

void PanelManager::onAttachedBarRevealSettled(wl_output* output, std::string_view barName) {
  if (!m_attachedOpenAnimationPending || !isAttachedOpen() || m_output != output) {
    return;
  }
  if (!m_sourceBarName.empty() && !barName.empty() && m_sourceBarName != barName) {
    return;
  }
  startAttachedOpenAnimation();
  requestFrameTick();
}

void PanelManager::registerPanel(const std::string& id, std::unique_ptr<Panel> content) {
  if (content != nullptr && content->isPersistent()) {
    m_persistentHost.registerPanel(id, std::move(content));
    return;
  }
  m_panels[id] = std::move(content);
}

void PanelManager::unregisterPanel(const std::string& id) {
  if (m_persistentHost.hasPanel(id)) {
    m_persistentHost.unregisterPanel(id);
    return;
  }
  auto it = m_panels.find(id);
  if (it == m_panels.end()) {
    return;
  }
  if (isOpenPanel(id)) {
    closePanel(/*animateClose=*/false);
  }
  m_panels.erase(it);
}

void PanelManager::openPanel(const std::string& panelId, PanelOpenRequest request) {
  if (m_inTransition) {
    return;
  }

  // A panel changing its own page retains its opener. Calendar week ->
  // month and Quick Settings subpages must not fall back to an edge-attached
  // panel with concave corners and an unrelated closing destination.
  std::string retainedSourceBar;
  if (isOpenPanel(panelId) && request.sourceBarName.empty() && !request.source.valid()) {
    retainedSourceBar = m_sourceBarName;
    request.sourceBarName = retainedSourceBar;
    request.source = m_attachedSource;
    request.output = request.output ? request.output : m_output;
    request.anchorX = m_attachedAnchorX;
    request.anchorY = m_attachedAnchorY;
    request.hasAnchorPosition = m_attachedAnchorAvailable;
  }

  if (request.output == nullptr && m_platform != nullptr) {
    request.output = m_platform->focusedInteractiveOutput(std::chrono::milliseconds(1200));
    if (request.output == nullptr) {
      // No focus source resolved an output (e.g. a compositor with no focus
      // IPC/backend). Ask the compositor which output an unpinned surface lands
      // on — the focused one — then reopen with that concrete output so all the
      // normal placement (attached, bar-relative, per-output config) applies.
      // Falls back to the arbitrary first output if the probe times out.
      //
      // The open is deferred past this call, so the request's string_view fields
      // are copied into owned storage the continuation keeps alive.
      m_platform->probeFocusedOutput(
          [this, panelId, request, context = std::string(request.context),
           sourceBarName = std::string(request.sourceBarName)](wl_output* probed) mutable {
            request.output =
                probed != nullptr ? probed : m_platform->preferredInteractiveOutput(std::chrono::milliseconds(1200));
            if (request.output == nullptr) {
              return; // no usable output at all — nothing to open on.
            }
            request.context = context;
            request.sourceBarName = sourceBarName;
            openPanel(panelId, request);
          },
          std::chrono::milliseconds(250)
      );
      return;
    }
  }

  // Persistent panels live in their own host: opening one must leave the active
  // panel (and any other persistent panel) alone.
  if (m_persistentHost.hasPanel(panelId)) {
    m_persistentHost.open(panelId, request.output, request.context);
    return;
  }

  if (m_closeDesktopWidgetsEditor) {
    m_closeDesktopWidgetsEditor();
  }

  // If a panel is open or closing, destroy it immediately with no close animation.
  // Bump the generation first so any in-flight deferred destroyPanel is a no-op.
  if (isOpen() || m_closing) {
    ++m_destroyGeneration;
    m_closing = false;
    destroyPanel();
  }

  auto it = m_panels.find(panelId);
  if (it == m_panels.end()) {
    kLog.warn("panel manager: unknown panel \"{}\"", panelId);
    return;
  }

  auto barConfigOpt = resolvePanelBarConfig(m_config, m_platform, request.output, request.sourceBarName);
  if (!barConfigOpt.has_value()) {
    return;
  }
  auto barConfig = std::move(*barConfigOpt);

  m_activePanel = it->second.get();
  m_activePanelId = panelId;
  m_activePanel->setContentScale(shell::panel_surface::contentScale(m_config));
  m_pendingOpenContext = std::string(request.context);
  m_activePanel->setPendingOpenContext(request.context);

  auto panelWidth = static_cast<std::uint32_t>(preferredPanelWidth());
  auto panelHeight = static_cast<std::uint32_t>(preferredPanelHeight());
  m_sourceBarName = barConfig.name;
  m_attachedSource = request.source;
  if (m_attachedSource.section == AttachedPanelSourceSection::Unknown
      && m_attachedSource.sectionId.empty()
      && m_config) {
    const auto prefix = panelId + "=";
    for (const auto& route : m_config->config().shell.panel.sourceSections) {
      if (!route.starts_with(prefix))
        continue;
      const auto id = route.substr(prefix.size());
      if (std::ranges::any_of(barConfig.sections, [&](const auto& section) { return section.id == id; }))
        m_attachedSource.sectionId = id;
      break;
    }
  }
  if (barConfig.sectionBackgrounds && barConfig.islandMorph && m_attachedSourceGeometryProvider) {
    if (auto source = m_attachedSourceGeometryProvider(request.output, m_sourceBarName, m_attachedSource);
        source && source->valid())
      m_attachedSource = *source;
  }
  m_islandMorph = barConfig.sectionBackgrounds && barConfig.islandMorph && m_attachedSource.valid();
  m_openingSourceBarName = std::string(request.sourceBarName);
  m_attachedAnchorAvailable = request.hasAnchorPosition;
  m_attachedAnchorX = request.anchorX;
  m_attachedAnchorY = request.anchorY;
  if (m_attachedPanelLayerProvider != nullptr) {
    if (auto layer = m_attachedPanelLayerProvider(request.output, m_sourceBarName); layer.has_value()) {
      barConfig.layer = *layer;
    }
  }
  const bool isBottom = barConfig.position == "bottom";
  const bool isLeft = barConfig.position == "left";
  const bool isRight = barConfig.position == "right";
  const std::int32_t panelGap = m_config->config().shell.panel.floatingOffset;
  const auto screenPadding = static_cast<std::int32_t>(Style::spaceSm);

  std::int32_t resolvedOutputWidth = 0;
  std::int32_t resolvedOutputHeight = 0;
  if (m_platform != nullptr) {
    const auto* wlOutput = m_platform->findOutputByWl(request.output);
    if (wlOutput != nullptr && wlOutput->effectiveLogicalWidth() > 0) {
      resolvedOutputWidth = wlOutput->effectiveLogicalWidth();
    }
    if (wlOutput != nullptr && wlOutput->effectiveLogicalHeight() > 0) {
      resolvedOutputHeight = wlOutput->effectiveLogicalHeight();
    }
  }
  // Backstop clamp: never request a surface larger than the output — the
  // compositor renders such a surface broken. This is sanity capping, not
  // work-area layout; if the compositor still configures smaller (exclusive
  // zones), buildScene lays out at the configured size.
  if (resolvedOutputWidth > 0) {
    panelWidth = std::min(panelWidth, static_cast<std::uint32_t>(std::max(1, resolvedOutputWidth - screenPadding * 2)));
  }
  if (resolvedOutputHeight > 0) {
    panelHeight =
        std::min(panelHeight, static_cast<std::uint32_t>(std::max(1, resolvedOutputHeight - screenPadding * 2)));
  }
  const std::int32_t outputWidth =
      resolvedOutputWidth > 0 ? resolvedOutputWidth : static_cast<std::int32_t>(panelWidth);
  const std::int32_t outputHeight =
      resolvedOutputHeight > 0 ? resolvedOutputHeight : static_cast<std::int32_t>(panelHeight);

  const auto clampMargin = [](float desired, std::int32_t panelSize, std::int32_t outputSize,
                              std::int32_t padding) -> std::int32_t {
    const std::int32_t maxValue = std::max(padding, outputSize - panelSize - padding);
    return static_cast<std::int32_t>(std::clamp(desired, static_cast<float>(padding), static_cast<float>(maxValue)));
  };

  const PanelPlacement activePlacement = m_activePanel->panelPlacement();
  // Fill sizing is floating-only (see Panel::fillsWidth): every other placement sizes the
  // surface from the panel's preferred extent.
  const bool floatingPlacement = activePlacement == PanelPlacement::Floating;
  const bool fillWidth = m_activePanel->fillsWidth() && floatingPlacement;
  const bool fillHeight = m_activePanel->fillsHeight() && floatingPlacement;
  if (!floatingPlacement && (m_activePanel->fillsWidth() || m_activePanel->fillsHeight())) {
    kLog.warn(
        "panel manager: \"{}\" uses fill sizing, which only applies to floating placement; opening at its preferred "
        "size",
        panelId
    );
  }
  m_panelFillWidth = fillWidth;
  m_panelFillHeight = fillHeight;
  const bool pluginPanel = m_activePanelId.contains(':');
  const std::string panelPosition =
      pluginPanel ? m_activePanel->panelScreenPosition() : resolvePanelPosition(m_config, m_activePanelId);
  const AttachedRevealDirection detachedDirection = detachedRevealDirection(panelPosition, barConfig.position);
  const bool useScreenPosition =
      activePlacement == PanelPlacement::Floating && panelPosition != "auto" && panelPosition != "center";
  const bool useCenterScreenLayout = (activePlacement == PanelPlacement::Floating && panelPosition == "center")
      || (activePlacement == PanelPlacement::Attached
          && m_attachedPanelAvailabilityCallback != nullptr
          && !m_attachedPanelAvailabilityCallback(request.output, m_sourceBarName));
  const bool useFloatingAnchor = !useCenterScreenLayout
      && request.hasAnchorPosition
      && openNearClickEnabled(m_activePanel, m_activePanelId, m_config);
  const auto detachedShadowBleed =
      shell::panel_surface::bleed(m_activePanel->hasDecoration(), m_config->config().shell.shadow);
  const std::uint32_t detachedSurfaceWidth =
      shell::panel_surface::surfaceExtent(panelWidth, detachedShadowBleed.left, detachedShadowBleed.right);
  const std::uint32_t detachedSurfaceHeight =
      shell::panel_surface::surfaceExtent(panelHeight, detachedShadowBleed.up, detachedShadowBleed.down);
  const auto barRect = resolveBarVisibleRect(barConfig, outputWidth, outputHeight);
  const bool multipleBarsOnEdge =
      hasMultipleEnabledBarsOnEdge(m_config, m_platform, request.output, barConfig.position);
  const bool useReservedEdgePlacement = !useCenterScreenLayout
      && !useScreenPosition
      && multipleBarsOnEdge
      && barConfig.reserveSpace
      && barConfig.thickness > 0;
  const auto marginLeftFromAnchor = clampMargin(
      request.anchorX - static_cast<float>(panelWidth) * 0.5F, static_cast<std::int32_t>(panelWidth), outputWidth,
      screenPadding
  );
  const auto marginTopFromAnchor = clampMargin(
      request.anchorY - static_cast<float>(panelHeight) * 0.5F, static_cast<std::int32_t>(panelHeight), outputHeight,
      screenPadding
  );

  std::uint32_t standaloneAnchor = 0;
  std::int32_t standaloneMarginTop = 0;
  std::int32_t standaloneMarginRight = 0;
  std::int32_t standaloneMarginBottom = 0;
  std::int32_t standaloneMarginLeft = 0;
  if (!useCenterScreenLayout) {
    const std::int32_t barWidth = std::max(0, barRect.right - barRect.left);
    const std::int32_t barHeight = std::max(0, barRect.bottom - barRect.top);
    const auto centeredAlongBarX = clampMargin(
        static_cast<float>(barRect.left) + (static_cast<float>(barWidth) - static_cast<float>(panelWidth)) * 0.5F,
        static_cast<std::int32_t>(panelWidth), outputWidth, screenPadding
    );
    const auto centeredAlongBarY = clampMargin(
        static_cast<float>(barRect.top) + (static_cast<float>(barHeight) - static_cast<float>(panelHeight)) * 0.5F,
        static_cast<std::int32_t>(panelHeight), outputHeight, screenPadding
    );

    if (useScreenPosition) {
      // Pinned to a screen edge/corner, independent of the bar.
      const auto sp = shell::screenPositionAnchor(panelPosition, panelGap);
      standaloneAnchor = sp.anchor;
      standaloneMarginTop = sp.marginTop;
      standaloneMarginRight = sp.marginRight;
      standaloneMarginBottom = sp.marginBottom;
      standaloneMarginLeft = sp.marginLeft;
    } else if (useReservedEdgePlacement) {
      if (isLeft) {
        standaloneAnchor = LayerShellAnchor::Left | LayerShellAnchor::Top;
        standaloneMarginLeft = panelGap;
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isRight) {
        standaloneAnchor = LayerShellAnchor::Right | LayerShellAnchor::Top;
        standaloneMarginRight = panelGap;
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isBottom) {
        standaloneAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
        standaloneMarginBottom = panelGap;
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      } else {
        standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
        standaloneMarginTop = panelGap;
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      }
    } else {
      standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
      if (isLeft) {
        standaloneMarginLeft = clampMargin(
            static_cast<float>(barRect.right + panelGap), static_cast<std::int32_t>(panelWidth), outputWidth,
            screenPadding
        );
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isRight) {
        standaloneMarginLeft = clampMargin(
            static_cast<float>(barRect.left - static_cast<std::int32_t>(panelWidth) - panelGap),
            static_cast<std::int32_t>(panelWidth), outputWidth, screenPadding
        );
        standaloneMarginTop = useFloatingAnchor ? marginTopFromAnchor : centeredAlongBarY;
      } else if (isBottom) {
        standaloneMarginTop = clampMargin(
            static_cast<float>(barRect.top - static_cast<std::int32_t>(panelHeight) - panelGap),
            static_cast<std::int32_t>(panelHeight), outputHeight, screenPadding
        );
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      } else {
        standaloneMarginTop = clampMargin(
            static_cast<float>(barRect.bottom + panelGap), static_cast<std::int32_t>(panelHeight), outputHeight,
            screenPadding
        );
        standaloneMarginLeft = useFloatingAnchor ? marginLeftFromAnchor : centeredAlongBarX;
      }
    }
  }

  if (useCenterScreenLayout) {
    standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    standaloneMarginLeft = (outputWidth - static_cast<std::int32_t>(panelWidth)) / 2 - detachedShadowBleed.left;
    standaloneMarginTop = (outputHeight - static_cast<std::int32_t>(panelHeight)) / 2 - detachedShadowBleed.up;
  } else {
    if ((standaloneAnchor & LayerShellAnchor::Left) != 0) {
      standaloneMarginLeft -= detachedShadowBleed.left;
    } else if ((standaloneAnchor & LayerShellAnchor::Right) != 0) {
      standaloneMarginRight -= detachedShadowBleed.right;
    }
    if ((standaloneAnchor & LayerShellAnchor::Top) != 0) {
      standaloneMarginTop -= detachedShadowBleed.up;
    } else if ((standaloneAnchor & LayerShellAnchor::Bottom) != 0) {
      standaloneMarginBottom -= detachedShadowBleed.down;
    }
  }

  InputRect detachedPanelInputRect = panelInputRectForSurface(
      standaloneAnchor, standaloneMarginTop, standaloneMarginRight, standaloneMarginBottom, standaloneMarginLeft,
      outputWidth, outputHeight, detachedSurfaceWidth, detachedSurfaceHeight, detachedShadowBleed.left,
      detachedShadowBleed.up, panelWidth, panelHeight
  );

  // Single-bar detached panels are placed relative to the bar's config edge. Honor
  // other surfaces' exclusive zones (exclusive_zone = 0 below) and anchor to the
  // bar's reserved edge so the panel tracks the bar's real on-screen position;
  // subtract the bar's own reservation on the main axis to avoid double-counting.
  // Reproduces the prior absolute placement when nothing else reserves space.
  const bool useBarRelativeDetached = !useCenterScreenLayout && !useScreenPosition && !useReservedEdgePlacement;
  if (useBarRelativeDetached) {
    const std::int32_t barReserved =
        barConfig.reserveSpace ? reservedBarEdgeDistance(barConfig, m_config->config().shell.shadow) : 0;
    const auto sw = static_cast<std::int32_t>(detachedSurfaceWidth);
    const auto sh = static_cast<std::int32_t>(detachedSurfaceHeight);
    if (isBottom) {
      standaloneAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
      standaloneMarginBottom = outputHeight - sh - standaloneMarginTop - barReserved;
      standaloneMarginTop = 0;
    } else if (isRight) {
      standaloneAnchor = LayerShellAnchor::Top | LayerShellAnchor::Right;
      standaloneMarginRight = outputWidth - sw - standaloneMarginLeft - barReserved;
      standaloneMarginLeft = 0;
    } else if (isLeft) {
      standaloneMarginLeft -= barReserved;
    } else {
      standaloneMarginTop -= barReserved;
    }
  }

  // A filled axis dual-anchors the surface with a requested size of 0: the
  // compositor assigns the extent, subtracting every exclusive zone on the
  // output (all bars and any third-party client) — the shell never computes
  // the work area itself. Margins keep the screen padding around the visible
  // body (the shadow bleed sits outside the padding); they override whatever
  // the placement branches above computed on that axis. The default size is
  // only the fallback if the compositor assigns nothing.
  std::uint32_t requestedSurfaceWidth = detachedSurfaceWidth;
  std::uint32_t requestedSurfaceHeight = detachedSurfaceHeight;
  std::uint32_t fallbackSurfaceWidth = detachedSurfaceWidth;
  std::uint32_t fallbackSurfaceHeight = detachedSurfaceHeight;
  if (fillWidth) {
    standaloneAnchor |= LayerShellAnchor::Left | LayerShellAnchor::Right;
    standaloneMarginLeft = screenPadding - detachedShadowBleed.left;
    standaloneMarginRight = screenPadding - detachedShadowBleed.right;
    requestedSurfaceWidth = 0;
    fallbackSurfaceWidth =
        static_cast<std::uint32_t>(std::max(1, outputWidth - standaloneMarginLeft - standaloneMarginRight));
    detachedPanelInputRect.x = screenPadding;
    detachedPanelInputRect.width = std::max(1, outputWidth - screenPadding * 2);
  }
  if (fillHeight) {
    standaloneAnchor |= LayerShellAnchor::Top | LayerShellAnchor::Bottom;
    standaloneMarginTop = screenPadding - detachedShadowBleed.up;
    standaloneMarginBottom = screenPadding - detachedShadowBleed.down;
    requestedSurfaceHeight = 0;
    fallbackSurfaceHeight =
        static_cast<std::uint32_t>(std::max(1, outputHeight - standaloneMarginTop - standaloneMarginBottom));
    detachedPanelInputRect.y = screenPadding;
    detachedPanelInputRect.height = std::max(1, outputHeight - screenPadding * 2);
  }

  const bool useRetainedPlacement = activePlacement == PanelPlacement::Attached
      && (m_attachedPanelAvailabilityCallback == nullptr
          || m_attachedPanelAvailabilityCallback(request.output, m_sourceBarName))
      && barConfig.thickness > 0
      && outputWidth > 0
      && outputHeight > 0;
  const LayerShellLayer floatingPanelLayer = resolveFloatingPanelLayer(m_config, m_activePanelId, *m_activePanel);
  const LayerShellLayer panelLayer =
      useRetainedPlacement ? layerShellLayerFromConfig(barConfig.layer) : floatingPanelLayer;
  m_panelLayer = panelLayer;

  const bool hasFocusGrab =
      m_platform != nullptr && m_platform->focusGrabService() != nullptr && m_platform->focusGrabService()->available();
  // Neither outside-click mechanism works for a panel that must not take keyboard
  // focus: the click shield swallows the click meant for the app below, and the
  // compositor focus grab takes the keyboard.
  const bool wantsOutsideDismiss =
      m_activePanel->dismissOnOutsideClick() && m_activePanel->keyboardMode() != LayerShellKeyboard::None;
  // Two plans: the attached branch falls back to the standalone surface when its
  // layer-shell init fails, and that surface is placed detached.
  const PanelKeyboardPlan keyboardPlan =
      resolvePanelKeyboardPlan(m_activePanel->keyboardMode(), hasFocusGrab, hasFocusGrab && wantsOutsideDismiss, false);
  const PanelKeyboardPlan attachedKeyboardPlan =
      resolvePanelKeyboardPlan(m_activePanel->keyboardMode(), hasFocusGrab, hasFocusGrab && wantsOutsideDismiss, true);

  if (wantsOutsideDismiss) {
    activateClickShield(panelLayer);
  }

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-panel",
      .layer = floatingPanelLayer,
      .anchor = standaloneAnchor,
      .width = requestedSurfaceWidth,
      .height = requestedSurfaceHeight,
      // Floating panels at the center position ignore exclusive zones; filled axes
      // must respect them so the compositor subtracts bars and other clients.
      .exclusiveZone = useCenterScreenLayout && !fillWidth && !fillHeight ? -1 : 0,
      .marginTop = standaloneMarginTop,
      .marginRight = standaloneMarginRight,
      .marginBottom = standaloneMarginBottom,
      .marginLeft = standaloneMarginLeft,
      .keyboard = keyboardPlan.initial,
      .defaultWidth = fallbackSurfaceWidth,
      .defaultHeight = fallbackSurfaceHeight,
      .prewarmBlur = true,
  };

  const auto configureSurfaceCallbacks = [this](Surface& surface) {
    surface.setRenderContext(m_renderContext);
    surface.setConfigureCallback([this](std::uint32_t, std::uint32_t) { onSurfaceConfigured(); });
    surface.setPrepareFrameCallback([this](bool needsUpdate, bool needsLayout) {
      prepareFrame(needsUpdate, needsLayout);
    });
    surface.setFrameTickCallback([this](float deltaMs) {
      startAttachedOpenAnimation();
      if (m_activePanel != nullptr) {
        m_activePanel->onFrameTick(deltaMs);
      }
    });
    surface.setAnimationManager(&m_animations);
  };

  const auto resetPanelOpenState = [this]() {
    deactivateOutsideClickHandlers();
    m_surface.reset();
    m_layerSurface = nullptr;
    m_output = nullptr;
    m_wlSurface = nullptr;
    m_panelLayer = LayerShellLayer::Top;
    m_activePanel = nullptr;
    m_activePanelId.clear();
    m_pendingOpenContext.clear();
    m_panelInsetX = 0;
    m_panelInsetY = 0;
    m_panelVisualWidth = 0;
    m_panelVisualHeight = 0;
    m_panelOutputInputRect.reset();
    m_panelFillWidth = false;
    m_panelFillHeight = false;
    m_detachedBleedRight = 0;
    m_detachedBleedBottom = 0;
    m_attachedBackgroundOpacity = 1.0F;
    m_attachedContactShadow = false;
    m_attachedRevealProgress = 1.0F;
    m_detachedRevealProgress = 1.0F;
    m_attachedRevealDirection = AttachedRevealDirection::Down;
    m_detachedRevealDirection = AttachedRevealDirection::Down;
    m_keyboardRelaxTimer.stop();
    m_attachedBarPosition.clear();
    m_sourceBarName.clear();
    m_attachedSource = {};
    m_attachedPanelGeometry.reset();
    m_attachedToBar = false;
    ++m_attachedPlacementGeneration;
    m_attachedPlacement.reset();
    m_attachedPlacementPending = m_attachedAwaitingConfigure = m_sceneGeometryDirty = false;
    m_openingSourceBarName.clear();
    m_attachedAnchorAvailable = m_attachedHasAnchor = false;
    m_attachedOpenAnimationPending = false;
  };

  if (useRetainedPlacement) {
    const std::string_view barPosition = barConfig.position;
    const bool barIsBottom = barPosition == "bottom";
    const bool barIsLeft = barPosition == "left";
    const bool barIsRight = barPosition == "right";
    const bool barIsVertical = barIsLeft || barIsRight;

    const float scale = m_activePanel->contentScale();
    const float cornerRadius = Style::scaledRadiusXl(scale);
    const auto& shadowConfig = m_config->config().shell.shadow;
    const auto shadowBleed = shell::surface_shadow::bleed(m_activePanel->hasDecoration(), shadowConfig, "panel", "panel");
    const auto cornerOutset = static_cast<std::int32_t>(std::ceil(cornerRadius));

    // Cross-axis outset wraps the concave-corner overhang and shadow bleed.
    // Main-axis bleed extends only away from the bar edge.
    std::int32_t crossOutsetStart = 0;
    std::int32_t crossOutsetEnd = 0;
    std::int32_t mainBleedAway = 0;
    if (barIsVertical) {
      crossOutsetStart = std::max(shadowBleed.up, shadowBleed.down) + cornerOutset + 2;
      crossOutsetEnd = crossOutsetStart;
      mainBleedAway = (barIsLeft ? shadowBleed.right : shadowBleed.left) + 2;
    } else {
      crossOutsetStart = std::max(shadowBleed.left, shadowBleed.right) + cornerOutset + 2;
      crossOutsetEnd = crossOutsetStart;
      mainBleedAway = (barIsBottom ? shadowBleed.up : shadowBleed.down) + 2;
    }

    m_attachedAnchorAvailable = request.hasAnchorPosition;
    m_attachedHasAnchor = m_attachedAnchorAvailable && openNearClickEnabled(m_activePanel, m_activePanelId, m_config);
    m_attachedAnchorX = request.anchorX;
    m_attachedAnchorY = request.anchorY;
    const int startRadius = barIsVertical ? (barIsLeft ? barConfig.radiusTopRight : barConfig.radiusTopLeft)
                                         : (barIsBottom ? barConfig.radiusTopLeft : barConfig.radiusBottomLeft);
    const int endRadius = barIsVertical ? (barIsLeft ? barConfig.radiusBottomRight : barConfig.radiusBottomLeft)
                                       : (barIsBottom ? barConfig.radiusTopRight : barConfig.radiusBottomRight);
    auto body = attached_panel::fitBody(attached_panel::revealDirection(barPosition),
        {barRect.left, barRect.top, barRect.right - barRect.left, barRect.bottom - barRect.top},
        outputWidth, outputHeight, static_cast<int>(panelWidth), static_cast<int>(panelHeight), screenPadding,
        barConfig.panelOverlap, static_cast<int>(std::ceil(startRadius + cornerRadius)),
        static_cast<int>(std::ceil(endRadius + cornerRadius)),
        m_attachedHasAnchor ? std::optional<float>(barIsVertical ? request.anchorY : request.anchorX) : std::nullopt);
    if (m_islandMorph) {
      body = barConfig.islandOutward ? attached_panel::fitOutwardIsland(
          attached_panel::revealDirection(barPosition),
          {barRect.left, barRect.top, barRect.right - barRect.left, barRect.bottom - barRect.top},
          m_attachedSource, outputWidth, outputHeight, static_cast<int>(panelWidth), static_cast<int>(panelHeight),
          screenPadding, barConfig.islandPanelOffset)
          : attached_panel::includeIslandSource(body, m_attachedSource, outputWidth, outputHeight);
    }
    panelWidth = static_cast<std::uint32_t>(body.width);
    panelHeight = static_cast<std::uint32_t>(body.height);

    const auto crossPad = static_cast<std::uint32_t>(std::max(0, crossOutsetStart + crossOutsetEnd));
    const int mainBleedToward=m_islandMorph ? 2+(barIsVertical
        ? (barIsLeft?shadowBleed.left:shadowBleed.right)
        : (barIsBottom?shadowBleed.down:shadowBleed.up)) : 0;
    const auto mainPad = static_cast<std::uint32_t>(std::max(0, mainBleedAway+mainBleedToward));
    const std::uint32_t surfaceWidth = barIsVertical ? (panelWidth + mainPad) : (panelWidth + crossPad);
    const std::uint32_t surfaceHeight = barIsVertical ? (panelHeight + crossPad) : (panelHeight + mainPad);

    // Bar visible rect in screen coords, derived from BarConfig + output dimensions.
    const std::int32_t mEnds = std::max(0, barConfig.marginEnds);
    const std::int32_t barLeft = barRect.left;
    const std::int32_t barTop = barRect.top;

    const std::int32_t visualX = body.x;
    const std::int32_t visualY = body.y;

    // Surface origin: cross-axis outset on each side, main-axis bleed on the side opposite the bar.
    std::int32_t surfaceX = 0;
    std::int32_t surfaceY = 0;
    if (barIsVertical) {
      surfaceY = visualY - crossOutsetStart;
      surfaceX = barIsLeft ? visualX-mainBleedToward : visualX - mainBleedAway;
    } else {
      surfaceX = visualX - crossOutsetStart;
      surfaceY = barIsBottom ? visualY - mainBleedAway : visualY-mainBleedToward;
    }

    m_panelInsetX = visualX - surfaceX;
    m_panelInsetY = visualY - surfaceY;
    m_attachedBleedRight=static_cast<std::int32_t>(surfaceWidth-panelWidth)-m_panelInsetX;
    m_attachedBleedBottom=static_cast<std::int32_t>(surfaceHeight-panelHeight)-m_panelInsetY;
    m_panelVisualWidth = panelWidth;
    m_panelVisualHeight = panelHeight;
    m_attachedBackground = barConfig.background;
    m_attachedBackgroundOpacity = m_activePanel->inheritsBarBackgroundOpacity()
        ? barConfig.backgroundOpacity
        : m_activePanel->attachedBackgroundOpacityOverride();
    m_attachedContactShadow = barConfig.contactShadow;
    m_attachedRevealProgress = 0.0F;
    m_attachedRevealDirection = attached_panel::revealDirection(barPosition);
    m_keyboardRelaxTimer.stop();
    m_attachedBarPosition = std::string(barPosition);
    m_attachedToBar = true;

    // Convert panel screen coords to bar-surface-local coords for shadow exclusion.
    // Bar surface origin sits one shadow bleed inset from the visible bar top-left,
    // plus the screen-edge concave flare: those corners push the surface further into
    // the end margin, and computeBarSurfaceSpec folds the same inset into its start
    // margin. Omitting it here drifts the exclusion rect along the main axis.
    const auto barShadowBleed = shell::surface_shadow::bleed(barConfig.shadow, shadowConfig, "bar", "bar");
    const auto barConcave = barConcaveShape(barConfig);
    const auto concaveStartInset = static_cast<std::int32_t>(
        std::ceil(std::max(0.0F, barIsVertical ? barConcave.logicalInset.top : barConcave.logicalInset.left))
    );
    std::int32_t barSurfaceLocalVisualX;
    std::int32_t barSurfaceLocalVisualY;
    if (barIsVertical) {
      barSurfaceLocalVisualY = visualY - (barTop - std::min(mEnds, barShadowBleed.up + concaveStartInset));
      const std::int32_t barSurfaceOriginX =
          barIsLeft ? std::max(0, barLeft - barShadowBleed.left) : barLeft - barShadowBleed.left;
      barSurfaceLocalVisualX = visualX - barSurfaceOriginX;
    } else {
      barSurfaceLocalVisualX = visualX - (barLeft - std::min(mEnds, barShadowBleed.left + concaveStartInset));
      const std::int32_t barSurfaceOriginY =
          barIsBottom ? barTop - barShadowBleed.up : std::max(0, barTop - barShadowBleed.up);
      barSurfaceLocalVisualY = visualY - barSurfaceOriginY;
    }

    // Geometry passed to the bar for shadow exclusion in bar-surface-local coords.
    // Visible rect extends past the body by cornerRadius on the cross axis.
    AttachedPanelGeometry attachedGeometry;
    attachedGeometry.cornerPower = m_sceneRoot ? m_sceneRoot->cornerPower() : Style::cornerPower;
    attachedGeometry.cornerRadius = cornerRadius;
    attachedGeometry.bulgeRadius = cornerRadius;
    attachedGeometry.source = m_attachedSource;
    attachedGeometry.revealProgress = 0.0F;
    if (barIsVertical) {
      attachedGeometry.x = static_cast<float>(barSurfaceLocalVisualX);
      attachedGeometry.y = static_cast<float>(barSurfaceLocalVisualY) - cornerRadius;
      attachedGeometry.width = static_cast<float>(panelWidth);
      attachedGeometry.height = static_cast<float>(panelHeight) + cornerRadius * 2.0F;
      attachedGeometry.outputX = static_cast<float>(visualX);
      attachedGeometry.outputY = static_cast<float>(visualY) - cornerRadius;
      attachedGeometry.outputWidth = attachedGeometry.width;
      attachedGeometry.outputHeight = attachedGeometry.height;
    } else {
      attachedGeometry.x = static_cast<float>(barSurfaceLocalVisualX) - cornerRadius;
      attachedGeometry.y = static_cast<float>(barSurfaceLocalVisualY);
      attachedGeometry.width = static_cast<float>(panelWidth) + cornerRadius * 2.0F;
      attachedGeometry.height = static_cast<float>(panelHeight);
      attachedGeometry.outputX = static_cast<float>(visualX) - cornerRadius;
      attachedGeometry.outputY = static_cast<float>(visualY);
      attachedGeometry.outputWidth = attachedGeometry.width;
      attachedGeometry.outputHeight = attachedGeometry.height;
    }
    if (m_islandMorph) {
      attachedGeometry.x=static_cast<float>(barSurfaceLocalVisualX);
      attachedGeometry.y=static_cast<float>(barSurfaceLocalVisualY);
      attachedGeometry.outputX=static_cast<float>(visualX);
      attachedGeometry.outputY=static_cast<float>(visualY);
      attachedGeometry.width=attachedGeometry.outputWidth=static_cast<float>(panelWidth);
      attachedGeometry.height=attachedGeometry.outputHeight=static_cast<float>(panelHeight);
      attachedGeometry.bulgeRadius=0.F;
    }
    attachedGeometry.finalOutputX = attachedGeometry.outputX;
    attachedGeometry.finalOutputY = attachedGeometry.outputY;
    attachedGeometry.finalOutputWidth = attachedGeometry.outputWidth;
    attachedGeometry.finalOutputHeight = attachedGeometry.outputHeight;
    m_attachedPanelGeometry = attachedGeometry;
    m_attachedBarOriginX=visualX-barSurfaceLocalVisualX;
    m_attachedBarOriginY=visualY-barSurfaceLocalVisualY;

    // Anchor against the bar's reserved edge and honor other surfaces' exclusive
    // zones (exclusive_zone = 0). The compositor stacks the panel past any external
    // reservation on that edge exactly as it does the bar, so the panel tracks the
    // bar's real on-screen position. surfaceX/surfaceY are computed from the bar's
    // config edge; subtracting the bar's own reservation on the main axis avoids
    // double-counting it. With no other reservation this matches the old absolute
    // placement; it self-corrects by the external reservation when one exists.
    const std::int32_t barReserved = barConfig.reserveSpace ? reservedBarEdgeDistance(barConfig, shadowConfig) : 0;
    std::uint32_t attachedAnchor = LayerShellAnchor::Top | LayerShellAnchor::Left;
    std::int32_t attachedMarginTop = surfaceY;
    std::int32_t attachedMarginRight = 0;
    std::int32_t attachedMarginBottom = 0;
    std::int32_t attachedMarginLeft = surfaceX;
    if (barIsBottom) {
      attachedAnchor = LayerShellAnchor::Bottom | LayerShellAnchor::Left;
      attachedMarginTop = 0;
      attachedMarginBottom = outputHeight - static_cast<std::int32_t>(surfaceHeight) - surfaceY - barReserved;
    } else if (barIsRight) {
      attachedAnchor = LayerShellAnchor::Top | LayerShellAnchor::Right;
      attachedMarginLeft = 0;
      attachedMarginRight = outputWidth - static_cast<std::int32_t>(surfaceWidth) - surfaceX - barReserved;
    } else if (barIsLeft) {
      attachedMarginLeft = surfaceX - barReserved;
    } else {
      attachedMarginTop = surfaceY - barReserved;
    }

    ++m_attachedPlacementGeneration;
    m_attachedPlacement = RetainedPlacement{
        .islandMorph = m_islandMorph,
        .anchoredRight = barIsRight, .anchoredBottom = barIsBottom,
        .barName = m_sourceBarName, .position = m_attachedBarPosition, .body = body,
        .insetX = m_panelInsetX, .insetY = m_panelInsetY,
        .trailingX = m_attachedBleedRight, .trailingY = m_attachedBleedBottom,
        .surfaceWidth = surfaceWidth, .surfaceHeight = surfaceHeight,
        .barOriginX = m_attachedBarOriginX, .barOriginY = m_attachedBarOriginY,
        .layer = panelLayer, .background = m_attachedBackground, .opacity = m_attachedBackgroundOpacity, .contactShadow = m_attachedContactShadow};
    m_attachedPlacementPending = true;
    m_attachedAwaitingConfigure = true;

    auto attachedConfig = LayerSurfaceConfig{
        .nameSpace = "noctalia-attached-panel",
        .layer = panelLayer,
        .anchor = attachedAnchor,
        .width = surfaceWidth,
        .height = surfaceHeight,
        .exclusiveZone = 0,
        .marginTop = attachedMarginTop,
        .marginRight = attachedMarginRight,
        .marginBottom = attachedMarginBottom,
        .marginLeft = attachedMarginLeft,
        .keyboard = attachedKeyboardPlan.initial,
        .defaultWidth = surfaceWidth,
        .defaultHeight = surfaceHeight,
        .prewarmBlur = true,
    };

    auto layerSurfaceUnique = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(attachedConfig));
    m_layerSurface = layerSurfaceUnique.get();
    m_surface = std::move(layerSurfaceUnique);
    configureSurfaceCallbacks(*m_surface);
    if (wantsOutsideDismiss) {
      m_panelOutputInputRect = InputRect{visualX, visualY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)};
      m_clickShield.setPanelInputRect(request.output, *m_panelOutputInputRect);
    }

    m_inTransition = true;
    const bool ok = m_layerSurface->initialize(request.output);
    m_inTransition = false;

    if (ok) {
      m_output = request.output;
      m_wlSurface = m_surface->wlSurface();
      m_surface->setInputRegion(
          {InputRect{m_panelInsetX, m_panelInsetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)}}
      );
      m_surface->setBlurRegion({});
      publishAttachedPanelGeometry(m_attachedRevealProgress);
      m_surface->requestRedraw();
      if (hasFocusGrab && wantsOutsideDismiss) {
        activateFocusGrab();
      }
      if (attachedKeyboardPlan.relaxed.has_value()) {
        const std::uint64_t gen = m_destroyGeneration;
        const LayerShellKeyboard relaxed = *attachedKeyboardPlan.relaxed;
        m_keyboardRelaxTimer.start(kKeyboardRelaxDelay, [this, gen, relaxed]() {
          if (m_destroyGeneration != gen || !isAttachedOpen() || m_layerSurface == nullptr || m_closing) {
            return;
          }
          m_layerSurface->setKeyboardInteractivity(relaxed);
        });
      }
      kLog.debug("panel manager: opened \"{}\" as attached layer-shell", panelId);
      if (m_panelOpenedCallback) {
        m_panelOpenedCallback();
      }
      return;
    }

    if (m_attachedPanelGeometryCallback) {
      m_attachedPanelGeometryCallback(request.output, m_sourceBarName, std::nullopt);
    }
    m_surface.reset();
    m_layerSurface = nullptr;
    m_attachedToBar = false;
    ++m_attachedPlacementGeneration;
    m_attachedPlacement.reset();
    m_attachedPlacementPending = m_attachedAwaitingConfigure = false;
    m_panelInsetX = 0;
    m_panelInsetY = 0;
    m_panelVisualWidth = 0;
    m_panelVisualHeight = 0;
    m_attachedBackgroundOpacity = 1.0F;
    m_attachedContactShadow = false;
    m_attachedRevealProgress = 1.0F;
    m_detachedRevealProgress = 1.0F;
    m_attachedRevealDirection = AttachedRevealDirection::Down;
    m_detachedRevealDirection = AttachedRevealDirection::Down;
    m_keyboardRelaxTimer.stop();
    m_attachedBarPosition.clear();
    m_attachedPanelGeometry.reset();
    m_attachedOpenAnimationPending = false;
    kLog.warn("panel manager: attached layer-shell failed for \"{}\", falling back to standalone", panelId);
    if (m_panelLayer != floatingPanelLayer) {
      m_clickShield.setLayer(floatingPanelLayer);
      m_panelLayer = floatingPanelLayer;
    }
  }

  auto layerSurface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  m_layerSurface = layerSurface.get();
  m_surface = std::move(layerSurface);
  m_panelInsetX = detachedShadowBleed.left;
  m_panelInsetY = detachedShadowBleed.up;
  m_panelVisualWidth = panelWidth;
  m_panelVisualHeight = panelHeight;
  m_detachedBleedRight = detachedShadowBleed.right;
  m_detachedBleedBottom = detachedShadowBleed.down;
  m_attachedBackgroundOpacity = 1.0F;
  m_attachedContactShadow = false;
  m_attachedRevealProgress = 1.0F;
  // This path publishes the compositor blur region before the first scene build.
  // Keep detached panels hidden until buildScene applies the opening reveal.
  m_detachedRevealProgress = 0.0F;
  m_attachedRevealDirection = AttachedRevealDirection::Down;
  m_detachedRevealDirection = detachedDirection;
  m_attachedPanelGeometry.reset();
  m_attachedToBar = false;
  configureSurfaceCallbacks(*m_surface);
  if (wantsOutsideDismiss) {
    m_panelOutputInputRect = detachedPanelInputRect;
    m_clickShield.setPanelInputRect(request.output, *m_panelOutputInputRect);
  }

  // Guard against re-entrancy: initialize can process queued Wayland events.
  m_inTransition = true;
  bool ok = m_layerSurface->initialize(request.output);
  m_inTransition = false;

  if (!ok) {
    kLog.warn("panel manager: failed to initialize surface for panel \"{}\"", panelId);
    resetPanelOpenState();
    return;
  }

  m_output = request.output;
  m_wlSurface = m_surface->wlSurface();
  m_surface->setInputRegion(
      {InputRect{m_panelInsetX, m_panelInsetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)}}
  );
  m_surface->setBlurRegion({});
  // Activate outside-click dismissal (focus grab or click shield).
  if (hasFocusGrab && wantsOutsideDismiss) {
    activateFocusGrab();
  }
  if (keyboardPlan.relaxed.has_value()) {
    const std::uint64_t gen = m_destroyGeneration;
    const LayerShellKeyboard relaxed = *keyboardPlan.relaxed;
    m_keyboardRelaxTimer.start(kKeyboardRelaxDelay, [this, gen, relaxed]() {
      if (m_destroyGeneration != gen || m_layerSurface == nullptr || m_closing) {
        return;
      }
      m_layerSurface->setKeyboardInteractivity(relaxed);
    });
  }
  kLog.debug("panel manager: opened \"{}\"", panelId);
  if (m_panelOpenedCallback) {
    m_panelOpenedCallback();
  }
}

void PanelManager::activateClickShield(LayerShellLayer layer) {
  if (m_activePanel == nullptr || m_platform == nullptr) {
    return;
  }
  // Hyprland: prefer the native focus-grab path. Skip the shield and let
  // activateFocusGrab handle it later.
  auto* grabService = m_platform->focusGrabService();
  if (grabService != nullptr && grabService->available()) {
    return;
  }
  std::vector<wl_output*> outputs;
  outputs.reserve(m_platform->outputs().size());
  for (const auto& wlOutput : m_platform->outputs()) {
    if (wlOutput.output != nullptr) {
      outputs.push_back(wlOutput.output);
    }
  }
  m_clickShield.activate(outputs, layer, m_clickShieldExcludeRectsProvider);
}

void PanelManager::activateFocusGrab() {
  if (m_platform == nullptr || m_wlSurface == nullptr) {
    return;
  }
  auto* grabService = m_platform->focusGrabService();
  if (grabService == nullptr || !grabService->available()) {
    return;
  }
  // Whitelist the panel and every bar surface. Clicks on whitelisted surfaces
  // pass through normally. Clicks anywhere else clear the grab and close the panel.
  m_focusGrab = grabService->createGrab();
  if (m_focusGrab == nullptr) {
    return;
  }
  m_focusGrab->setOnCleared([this]() {
    if (isOpen() && !m_closing) {
      closePanel();
    }
  });
  grabService->setPopupGrabHost(this);
  m_focusGrab->addSurface(m_wlSurface);
  if (m_focusGrabBarSurfacesProvider) {
    auto bars = m_focusGrabBarSurfacesProvider();
    for (auto* surface : bars) {
      m_focusGrab->addSurface(surface);
    }
  }
  m_focusGrab->commit();
}

void PanelManager::deactivateOutsideClickHandlers() {
  m_clickShield.deactivate();
  if (m_platform != nullptr) {
    if (auto* svc = m_platform->focusGrabService(); svc != nullptr && svc->popupGrabHost() == this) {
      svc->setPopupGrabHost(nullptr);
    }
  }
  m_focusGrab.reset();
}

void PanelManager::closePanel(bool animateClose) {
  if(m_resizeAnimationId)m_animations.cancel(m_resizeAnimationId);
  m_resizeAnimationId=0;m_resizeSize.reset();m_resizeTarget.reset();
  if (!isOpen() || m_inTransition || m_closing) {
    return;
  }

  kLog.debug("panel manager: closing \"{}\"", m_activePanelId);

  // Drop the outside-click handlers as soon as close starts.
  // During the close animation we want clicks on apps to behave normally.
  deactivateOutsideClickHandlers();

  // Disable input during close animation
  m_inputDispatcher.setSceneRoot(nullptr);
  m_closing = true;
  m_attachedOpenAnimationPending = false;

  if (animateClose && m_sceneRoot != nullptr && m_activePanel != nullptr && m_activePanel->wantsCloseAnimation()) {
    const std::uint64_t gen = ++m_destroyGeneration;
    if (m_attachedToBar && m_attachedRevealClipNode != nullptr) {
      m_animations.cancelForOwner(m_attachedRevealClipNode);
      m_animations.animate(
          m_attachedRevealProgress, 0.0F, attachedAnimationDuration(), Easing::EaseInOutQuad,
          [this](float v) { applyAttachedReveal(v); },
          [this, gen]() {
            DeferredCall::callLater([this, gen]() {
              if (m_destroyGeneration == gen) {
                destroyPanel();
              }
            });
          },
          m_attachedRevealClipNode
      );
    } else {
      m_animations.cancelForOwner(m_sceneRoot.get());
      m_animations.animate(
          m_detachedRevealProgress, 0.0F, Style::animNormal, Easing::EaseInQuad,
          [this](float v) { applyDetachedReveal(v); },
          [this, gen]() {
            DeferredCall::callLater([this, gen]() {
              if (m_destroyGeneration == gen) {
                destroyPanel();
              }
            });
          },
          m_sceneRoot.get()
      );
    }
    m_surface->requestRedraw();
  } else {
    destroyPanel();
  }
}

void PanelManager::destroyPanel() {
  if (m_attachedToBar && m_attachedPanelGeometryCallback && m_output != nullptr) {
    m_attachedPanelGeometryCallback(m_output, m_sourceBarName, std::nullopt);
  }
  // Defensive: closePanel deactivates first, but destroyPanel can also be
  // reached directly when openPanel preempts an open panel.
  deactivateOutsideClickHandlers();
  m_animations.cancelAll();
  m_resizeAnimationId=0;m_resizeSize.reset();m_resizeTarget.reset();
  m_closing = false;
  m_pointerInside = false;
  m_attachedPopupCount = 0;
  m_inputDispatcher.setSceneRoot(nullptr);
  // Hover leave only fades tooltips asynchronously. Destroy them (and any
  // open context menu) before the layer surface — xdg_popup must die first.
  TooltipManager::instance().restoreBarTooltipsForPanel(m_activePanelId);
  TooltipManager::instance().forceDestroy();
  if (m_activePopup != nullptr) {
    m_activePopup->close();
    m_activePopup = nullptr;
  }
  if (m_activePanel != nullptr) {
    m_activePanel->onClose();
  }
  m_bgNode = nullptr;
  m_contentNode = nullptr;
  m_detachedRevealClipNode = nullptr;
  m_detachedRevealContentNode = nullptr;
  m_attachedRevealClipNode = nullptr;
  m_attachedRevealContentNode = nullptr;
  m_attachedContentClipNode = nullptr;
  m_islandOpenerProxy = nullptr;
  m_islandUsageRing = nullptr;
  m_panelShadowNode = nullptr;
  m_panelContactShadowNode = nullptr;
  m_selectPopup.reset();
  m_sceneRoot.reset();
  m_surface.reset();
  m_layerSurface = nullptr;
  m_output = nullptr;
  m_wlSurface = nullptr;
  m_activePanel = nullptr;
  m_activePanelId.clear();
  m_pendingOpenContext.clear();
  m_panelInsetX = 0;
  m_panelInsetY = 0;
  m_panelVisualWidth = 0;
  m_panelVisualHeight = 0;
  m_panelOutputInputRect.reset();
  m_panelFillWidth = false;
  m_panelFillHeight = false;
  m_detachedBleedRight = 0;
  m_detachedBleedBottom = 0;
  m_attachedBackgroundOpacity = 1.0F;
  m_attachedContactShadow = false;
  m_attachedRevealProgress = 1.0F;
  m_detachedRevealProgress = 1.0F;
  m_attachedRevealDirection = AttachedRevealDirection::Down;
  m_detachedRevealDirection = AttachedRevealDirection::Down;
  m_keyboardRelaxTimer.stop();
  m_attachedBarPosition.clear();
  m_sourceBarName.clear();
  m_attachedSource = {};
  m_attachedPanelGeometry.reset();
  m_attachedToBar = false;
  ++m_attachedPlacementGeneration;
  m_attachedPlacement.reset();
  m_attachedPlacementPending = m_attachedAwaitingConfigure = m_sceneGeometryDirty = false;
  m_openingSourceBarName.clear();
  m_attachedAnchorAvailable = m_attachedHasAnchor = false;
  m_attachedOpenAnimationPending = false;
  if (m_platform != nullptr) {
    m_platform->stopKeyRepeat();
  }
  if (m_panelClosedCallback) {
    m_panelClosedCallback();
  }
}

void PanelManager::navigatePanelContext(const std::string& panelId, std::string context) {
  const auto generation = m_destroyGeneration;
  DeferredCall::callLater([this, panelId, context = std::move(context), generation] {
    if (generation != m_destroyGeneration) return;
    if (!isOpenPanel(panelId) || m_closing || !m_contentNode) {
      openPanel(panelId, {.context = context});
      return;
    }
    // Retire input before freeing the old page; keep the retained panel geometry.
    m_inputDispatcher.setSceneRoot(nullptr);
    TooltipManager::instance().forceDestroy();
    if (m_activePopup) { m_activePopup->close(); m_activePopup = nullptr; }
    m_selectPopup.reset();
    m_activePanel->onClose();
    while (!m_contentNode->children().empty())
      (void)m_contentNode->removeChild(m_contentNode->children().back().get());
    m_activePanel->setPendingOpenContext(context);
    m_activePanel->create();
    m_activePanel->onOpen(context);
    if (m_activePanel->root()) m_contentNode->addChild(m_activePanel->releaseRoot());
    m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
    if (auto* focus = m_activePanel->initialFocusArea()) m_inputDispatcher.setFocus(focus);
    relayoutActivePanelPreferredSize();
    m_surface->requestUpdate();
    m_surface->requestLayout();
  });
}

void PanelManager::togglePanel(const std::string& panelId, PanelOpenRequest request) {
  if (m_persistentHost.hasPanel(panelId)) {
    if (m_persistentHost.isOpen(panelId)) {
      m_persistentHost.close(panelId);
      return;
    }
    openPanel(panelId, request);
    return;
  }
  // Treat a closing panel as closed: re-clicking while it animates out reopens it immediately.
  if (isOpen() && !m_closing && m_activePanelId == panelId) {
    if (!request.context.empty() && m_activePanel != nullptr) {
      if (m_activePanel->isContextActive(request.context)) {
        closePanel();
        return;
      }
      // Panels placed near the clicked widget must fully reopen so geometry
      // and bar decoration track the new anchor.
      if (request.hasAnchorPosition && openNearClickEnabled(m_activePanel, panelId, m_config)) {
        openPanel(panelId, request);
        return;
      }
      m_activePanel->onOpen(request.context);
      refresh();
      return;
    }
    closePanel();
  } else {
    openPanel(panelId, request);
  }
}

void PanelManager::togglePanel(const std::string& panelId) {
  if (m_persistentHost.hasPanel(panelId)) {
    if (m_persistentHost.isOpen(panelId)) {
      m_persistentHost.close(panelId);
      return;
    }
    openPanel(panelId, PanelOpenRequest{});
    return;
  }
  if (isOpen() && !m_closing && m_activePanelId == panelId) {
    closePanel();
    return;
  }
  // Output left unset: openPanel resolves it (focus source, else compositor probe).
  openPanel(panelId, PanelOpenRequest{});
}

bool PanelManager::onPointerEvent(const PointerEvent& event) {
  // A context menu may belong to a persistent plugin panel, for which the
  // ordinary single-panel host is closed. Route the grabbing popup first.
  if (m_activePopup != nullptr) {
    if (m_activePopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_activePopup->close();
      return true;
    }
  }

  // Persistent panels own separate surfaces; the host claims only its own.
  if (m_persistentHost.onPointerEvent(event)) {
    return true;
  }
  if (!isOpen() || m_inTransition) {
    return false;
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    if (m_selectPopup->onPointerEvent(event)) {
      return true;
    }
    if (event.type == PointerEvent::Type::Button && event.pressed) {
      m_selectPopup->closeSelectDropdown();
      return true;
    }
  }

  if (m_attachedPopupCount > 0) {
    if (event.surface == m_wlSurface) {
      if (event.type == PointerEvent::Type::Enter) {
        m_pointerInside = true;
      } else if (event.type == PointerEvent::Type::Leave) {
        m_pointerInside = false;
      }
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    if (event.surface == m_wlSurface) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (event.surface == m_wlSurface) {
      m_pointerInside = false;
      m_inputDispatcher.pointerLeave();
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (!m_pointerInside) {
      return false;
    }
    m_inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    break;
  }
  case PointerEvent::Type::Button: {
    bool pressed = event.pressed;

    // Click outside panel closes it.
    if (pressed && !m_pointerInside) {
      closePanel();
      return false;
    }

    if (m_pointerInside) {
      if (pressed && event.surface == m_wlSurface && m_inputDispatcher.hoveredArea() == nullptr) {
        if (m_activePanel != nullptr && m_activePanel->dismissTransientUi()) {
          refresh();
          return true;
        }
      }
      m_inputDispatcher.pointerButton(
          static_cast<float>(event.sx), static_cast<float>(event.sy), event.button, pressed, event.serial, event.time,
          event.touch
      );
    }
    break;
  }
  case PointerEvent::Type::Axis: {
    if (!m_pointerInside) {
      return false;
    }
    m_inputDispatcher.pointerAxis(
        static_cast<float>(event.sx), static_cast<float>(event.sy), event.axis, event.axisSource, event.axisValue,
        event.axisDiscrete, event.axisValue120, event.axisLines
    );
    break;
  }
  }

  // Pointer interactions often only affect visual state.
  // Relayout only when the scene explicitly accumulated layout invalidation.
  if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty() && m_activePanel != nullptr && !m_activePanel->deferPointerRelayout()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }

  return m_pointerInside;
}

bool PanelManager::isOpen() const noexcept { return m_surface != nullptr && m_activePanel != nullptr; }

bool PanelManager::isOpenPanel(std::string_view panelId) const noexcept {
  if (m_persistentHost.hasPanel(panelId)) {
    return m_persistentHost.isOpen(panelId);
  }
  return isOpen() && m_activePanelId == panelId;
}

bool PanelManager::isPanelTransitionActive() const noexcept {
  if (!isOpen() && !m_closing) {
    return false;
  }
  if (m_closing || m_attachedOpenAnimationPending) {
    return true;
  }
  if (m_attachedToBar) {
    return m_attachedRevealProgress < 0.999F;
  }
  return m_detachedRevealProgress < 0.999F;
}

bool PanelManager::isAttachedOpen() const noexcept { return isOpen() && m_attachedToBar; }

wl_output* PanelManager::attachedPanelOutput() const noexcept { return m_output; }

std::string_view PanelManager::attachedSourceBarName() const noexcept { return m_sourceBarName; }

const std::string& PanelManager::activePanelId() const noexcept { return m_activePanelId; }

std::string_view PanelManager::activePanelContext() const noexcept {
  return isOpen() && m_activePanel != nullptr ? m_activePanel->activeContext() : std::string_view{};
}

bool PanelManager::isActivePanelContext(std::string_view context) const noexcept {
  if (!isOpen() || m_activePanel == nullptr) {
    return false;
  }
  return m_activePanel->isContextActive(context);
}

void PanelManager::refresh() {
  m_persistentHost.refresh();
  if (!isOpen() || m_renderContext == nullptr || m_activePanel == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_activePanel->deferExternalRefresh()) {
    return;
  }

  m_surface->requestUpdate();
}

float PanelManager::preferredPanelWidth() const {
  return m_resizeSize ? m_resizeSize->first : m_activePanel->preferredWidth();
}
float PanelManager::preferredPanelHeight() const {
  return m_resizeSize ? m_resizeSize->second : m_activePanel->preferredHeight();
}
void PanelManager::relayoutActivePanelPreferredSize() {
  if (!isOpen() || m_closing) return;
  const std::pair<float,float> target{m_activePanel->preferredWidth(),m_activePanel->preferredHeight()};
  if (m_resizeTarget == target) return;
  const std::pair<float,float> from=m_resizeSize.value_or(std::pair<float,float>{m_panelVisualWidth,m_panelVisualHeight});
  if (m_resizeAnimationId) m_animations.cancel(m_resizeAnimationId);
  m_resizeAnimationId=0;m_resizeTarget.reset();m_resizeSize.reset();
  const float duration=m_config ? m_config->config().shell.panel.resizeDurationMs : 180.F;
  if (!m_sceneRoot || !MotionService::instance().enabled() || duration<=0 || from.first<=0 || from.second<=0
      || m_attachedRevealProgress<.999F || m_detachedRevealProgress<.999F
      || (std::abs(from.first-target.first)<1.F && std::abs(from.second-target.second)<1.F)) {
    applyPreferredPanelSize();return;
  }
  m_resizeSize=from;m_resizeTarget=target;
  const auto generation=m_destroyGeneration;
  m_resizeAnimationId=m_animations.animate(0.F,1.F,duration,Easing::EaseOutCubic,
    [this,from,target,generation](float progress){
      if(generation!=m_destroyGeneration || !isOpen() || m_closing)return;
      m_resizeSize=std::pair{std::lerp(from.first,target.first,progress),std::lerp(from.second,target.second,progress)};
      applyPreferredPanelSize();
    },[this,generation]{
      if(generation!=m_destroyGeneration || !isOpen() || m_closing)return;
      m_resizeAnimationId=0;m_resizeSize.reset();m_resizeTarget.reset();applyPreferredPanelSize();
    },m_sceneRoot.get());
  requestFrameTick();
}

void PanelManager::applyPreferredPanelSize() {
  if (!isOpen() || m_activePanel == nullptr || m_surface == nullptr || m_layerSurface == nullptr) {
    return;
  }
  if (m_attachedToBar || m_attachedPlacement || m_activePanel->panelPlacement() == PanelPlacement::Attached) {
    // This path keeps the existing panel tree and resolves its body, join,
    // shadow, input and blur from the same current bar geometry.
    refreshPanelPlacement();
    return;
  }
  if (m_platform == nullptr || m_config == nullptr) {
    return;
  }

  const auto screenPadding = static_cast<std::int32_t>(Style::spaceSm);
  const auto* wlOutput = m_output != nullptr ? m_platform->findOutputByWl(m_output) : nullptr;
  const std::int32_t outputWidth = wlOutput != nullptr ? wlOutput->effectiveLogicalWidth() : 0;
  const std::int32_t outputHeight = wlOutput != nullptr ? wlOutput->effectiveLogicalHeight() : 0;

  auto panelWidth = static_cast<std::uint32_t>(std::max(1.0F, std::round(preferredPanelWidth())));
  auto panelHeight = static_cast<std::uint32_t>(std::max(1.0F, std::round(preferredPanelHeight())));
  if (outputWidth > 0) {
    panelWidth = std::min(panelWidth, static_cast<std::uint32_t>(std::max(1, outputWidth - screenPadding * 2)));
  }
  if (outputHeight > 0) {
    panelHeight = std::min(panelHeight, static_cast<std::uint32_t>(std::max(1, outputHeight - screenPadding * 2)));
  }
  const auto detachedShadowBleed =
      shell::panel_surface::bleed(m_activePanel->hasDecoration(), m_config->config().shell.shadow);
  const bool bleedChanged=m_panelInsetX!=detachedShadowBleed.left || m_panelInsetY!=detachedShadowBleed.up
      || m_detachedBleedRight!=detachedShadowBleed.right || m_detachedBleedBottom!=detachedShadowBleed.down;
  if(panelWidth==m_panelVisualWidth && panelHeight==m_panelVisualHeight && !bleedChanged)return;
  // Preserve the body's on-screen position while expanding transparent bounds.
  const auto anchor=m_layerSurface->anchor();
  m_layerSurface->setMargins(
      m_layerSurface->marginTop()+((anchor&LayerShellAnchor::Top)?m_panelInsetY-detachedShadowBleed.up:0),
      m_layerSurface->marginRight()+((anchor&LayerShellAnchor::Right)?m_detachedBleedRight-detachedShadowBleed.right:0),
      m_layerSurface->marginBottom()+((anchor&LayerShellAnchor::Bottom)?m_detachedBleedBottom-detachedShadowBleed.down:0),
      m_layerSurface->marginLeft()+((anchor&LayerShellAnchor::Left)?m_panelInsetX-detachedShadowBleed.left:0));
  m_panelInsetX=detachedShadowBleed.left;m_panelInsetY=detachedShadowBleed.up;
  m_detachedBleedRight=detachedShadowBleed.right;m_detachedBleedBottom=detachedShadowBleed.down;
  const std::uint32_t surfaceWidth =
      shell::panel_surface::surfaceExtent(panelWidth, detachedShadowBleed.left, detachedShadowBleed.right);
  const std::uint32_t surfaceHeight =
      shell::panel_surface::surfaceExtent(panelHeight, detachedShadowBleed.up, detachedShadowBleed.down);

  const std::string panelPosition = resolvePanelPosition(m_config, m_activePanelId);
  const bool useCenterScreenLayout =
      m_activePanel->panelPlacement() == PanelPlacement::Floating && panelPosition == "center";
  if (m_panelOutputInputRect.has_value()) {
    InputRect rect = *m_panelOutputInputRect;
    const std::uint32_t anchor = m_layerSurface->anchor();
    if (!m_panelFillWidth) {
      const auto widthDelta = static_cast<std::int32_t>(panelWidth) - static_cast<std::int32_t>(m_panelVisualWidth);
      const bool anchoredLeft = (anchor & LayerShellAnchor::Left) != 0;
      const bool anchoredRight = (anchor & LayerShellAnchor::Right) != 0;
      if (useCenterScreenLayout && outputWidth > 0) {
        rect.x = (outputWidth - static_cast<std::int32_t>(panelWidth)) / 2;
      } else if (anchoredRight && !anchoredLeft) {
        rect.x -= widthDelta;
      } else if (anchoredLeft == anchoredRight) {
        rect.x -= widthDelta / 2;
      }
      rect.width = static_cast<int>(panelWidth);
    }
    if (!m_panelFillHeight) {
      const auto heightDelta = static_cast<std::int32_t>(panelHeight) - static_cast<std::int32_t>(m_panelVisualHeight);
      const bool anchoredTop = (anchor & LayerShellAnchor::Top) != 0;
      const bool anchoredBottom = (anchor & LayerShellAnchor::Bottom) != 0;
      if (useCenterScreenLayout && outputHeight > 0) {
        rect.y = (outputHeight - static_cast<std::int32_t>(panelHeight)) / 2;
      } else if (anchoredBottom && !anchoredTop) {
        rect.y -= heightDelta;
      } else if (anchoredTop == anchoredBottom) {
        rect.y -= heightDelta / 2;
      }
      rect.height = static_cast<int>(panelHeight);
    }
    m_panelOutputInputRect = rect;
    m_clickShield.setPanelInputRect(m_output, rect);
  }

  m_panelVisualWidth = panelWidth;
  m_panelVisualHeight = panelHeight;

  if (useCenterScreenLayout && outputWidth > 0 && outputHeight > 0) {
    const std::int32_t marginLeft =
        (outputWidth - static_cast<std::int32_t>(panelWidth)) / 2 - detachedShadowBleed.left;
    const std::int32_t marginTop = (outputHeight - static_cast<std::int32_t>(panelHeight)) / 2 - detachedShadowBleed.up;
    m_layerSurface->setMargins(marginTop, m_layerSurface->marginRight(), m_layerSurface->marginBottom(), marginLeft);
  }

  m_layerSurface->requestSize(m_panelFillWidth?0:surfaceWidth,m_panelFillHeight?0:surfaceHeight);
  m_surface->setInputRegion(
      {InputRect{m_panelInsetX, m_panelInsetY, static_cast<int>(panelWidth), static_cast<int>(panelHeight)}}
  );
  m_surface->requestLayout();
}

void PanelManager::refreshPanel(std::string_view panelId) {
  if (m_persistentHost.hasPanel(panelId)) {
    m_persistentHost.refreshPanel(panelId);
    return;
  }
  if (isOpenPanel(panelId)) {
    refresh();
  }
}

void PanelManager::closePanelById(std::string_view panelId) {
  if (m_persistentHost.hasPanel(panelId)) {
    m_persistentHost.close(std::string(panelId));
    return;
  }
  if (isOpenPanel(panelId)) {
    closePanel();
  }
}

void PanelManager::requestAnimationFrameForPanel(std::string_view panelId) {
  if (m_persistentHost.hasPanel(panelId)) {
    m_persistentHost.requestAnimationFrame(panelId);
    return;
  }
  if (isOpenPanel(panelId)) {
    requestRedraw();
  }
}

void PanelManager::onIconThemeChanged() {
  m_persistentHost.onIconThemeChanged();
  if (!isOpen() || m_activePanel == nullptr || m_surface == nullptr) {
    return;
  }

  m_activePanel->onIconThemeChanged();
  m_surface->requestUpdate();
}

void PanelManager::focusArea(InputArea* area) {
  if (!isOpen() || m_sceneRoot == nullptr) {
    return;
  }
  m_inputDispatcher.setFocus(area);
}

void PanelManager::requestUpdateOnly() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestUpdateOnly();
}

void PanelManager::requestLayout() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestLayout();
}

void PanelManager::requestRedraw() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestRedraw();
}

void PanelManager::requestFrameTick() {
  if (!isOpen() || m_surface == nullptr) {
    return;
  }
  m_surface->requestFrameTick();
}

void PanelManager::close() { closePanel(); }

void PanelManager::configureContextMenuPopup(ContextMenuPopup& popup) const {
  if (m_config != nullptr) {
    popup.setShadowConfig(m_config->config().shell.shadow);
  }
}

void PanelManager::setActivePopup(ContextMenuPopup* popup) {
  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->closeSelectDropdown();
  }
  if (m_activePopup != nullptr && m_activePopup != popup) {
    m_activePopup->close();
  }
  m_activePopup = popup;
}

void PanelManager::clearActivePopup() { m_activePopup = nullptr; }

void PanelManager::registerPopupSurface(wl_surface* surface) {
  if (m_focusGrab == nullptr || surface == nullptr) {
    return;
  }
  m_focusGrab->addSurface(surface);
  m_focusGrab->commit();
}

void PanelManager::unregisterPopupSurface(wl_surface* surface) {
  if (m_focusGrab == nullptr || surface == nullptr) {
    return;
  }
  m_focusGrab->removeSurface(surface);
  m_focusGrab->commit();
}

void PanelManager::beginAttachedPopup(wl_surface* surface) {
  if (surface == nullptr || surface != m_wlSurface) {
    return;
  }
  ++m_attachedPopupCount;
}

void PanelManager::endAttachedPopup(wl_surface* surface) {
  if (surface == nullptr || surface != m_wlSurface) {
    return;
  }
  if (m_attachedPopupCount > 0) {
    --m_attachedPopupCount;
  }
  if (m_attachedPopupCount > 0) {
    return;
  }
  m_pointerInside =
      m_platform != nullptr && m_platform->hasPointerPosition() && m_platform->lastPointerSurface() == m_wlSurface;
  if (m_pointerInside) {
    m_inputDispatcher.pointerEnter(
        static_cast<float>(m_platform->lastPointerX()), static_cast<float>(m_platform->lastPointerY()),
        m_platform->lastInputSerial()
    );
  } else {
    m_inputDispatcher.pointerLeave();
  }
  requestRedraw();
}

std::optional<LayerPopupParentContext> PanelManager::popupParentContextForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr || surface != m_wlSurface) {
    return std::nullopt;
  }
  return fallbackPopupParentContext();
}

std::optional<LayerPopupParentContext> PanelManager::fallbackPopupParentContext() const noexcept {
  if (!isOpen() || m_surface == nullptr || m_wlSurface == nullptr || m_layerSurface == nullptr) {
    return std::nullopt;
  }

  LayerPopupParentContext context;
  context.surface = m_wlSurface;
  context.layerSurface = m_layerSurface->layerSurface();
  context.output = m_output;
  context.width = m_surface->width();
  context.height = m_surface->height();
  if (context.layerSurface == nullptr || context.width == 0 || context.height == 0) {
    return std::nullopt;
  }
  return context;
}

std::optional<LayerPopupParentContext>
PanelManager::popupParentContextForPanel(std::string_view panelId) const noexcept {
  if (m_persistentHost.hasPanel(panelId)) {
    return m_persistentHost.popupParentContext(panelId);
  }
  if (panelId != m_activePanelId) {
    return std::nullopt;
  }
  return fallbackPopupParentContext();
}

void PanelManager::onKeyboardEvent(const KeyboardEvent& event) {
  if (m_persistentHost.onKeyboardEvent(event)) {
    return;
  }
  // m_inTransition means the surface is still initializing.
  // Keyboard events during this window must be ignored.
  if (!isOpen() || m_inTransition) {
    return;
  }

  // Gate on compositor focus: route keys only when the surface owning this panel
  // input is the one the compositor reports as keyboard-focused.
  if (m_platform != nullptr) {
    wl_surface* const kbSurface = m_platform->lastKeyboardSurface();
    const bool onPanel = (m_wlSurface != nullptr && kbSurface == m_wlSurface);
    const bool onSelectPopup =
        (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen() && kbSurface == m_selectPopup->wlSurface());
    if (!onPanel && !onSelectPopup) {
      return;
    }
  }

  if (m_selectPopup != nullptr && m_selectPopup->isSelectDropdownOpen()) {
    m_selectPopup->onKeyboardEvent(event);
    return;
  }

  if (event.pressed && KeybindMatcher::matches(KeybindAction::Cancel, event.sym, event.modifiers)) {
    if (m_activePanel != nullptr
        && m_activePanel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
      if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else {
          m_surface->requestRedraw();
        }
      }
      return;
    }
    if (m_activePanel != nullptr && m_activePanel->dismissTransientUi()) {
      if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else {
          m_surface->requestRedraw();
        }
      }
      return;
    }
    closePanel();
    return;
  }

  // A focused text input owns plain printable keys; the panel's global key
  // handler must not claim them (Space is a Validate chord but must type a space).
  const InputArea* const focusedArea = m_inputDispatcher.focusedArea();
  const bool textInputFocused = focusedArea != nullptr && focusedArea->textInputClient() != nullptr;
  const bool reserveForTextInput =
      event.pressed && textInputFocused && isPlainPrintableKey(event.utf32, event.modifiers, event.preedit);

  if (!reserveForTextInput
      && m_activePanel != nullptr
      && m_activePanel->handleGlobalKey(event.sym, event.modifiers, event.pressed, event.preedit)) {
    if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
      if (m_sceneRoot->layoutDirty()) {
        m_surface->requestLayout();
      } else {
        m_surface->requestRedraw();
      }
    }
    return;
  }

  bool directional = false;
  if (m_activePanel != nullptr && m_activePanel->directionalNavigation() && !textInputFocused &&
      !event.preedit && event.modifiers == 0) {
    const int dx = KeySymbol::isRight(event.sym) ? 1 : KeySymbol::isLeft(event.sym) ? -1 : 0;
    const int dy = KeySymbol::isDown(event.sym) ? 1 : KeySymbol::isUp(event.sym) ? -1 : 0;
    bool slider = false;
    for (const Node* node=focusedArea; node!=nullptr; node=node->parent())
      if (dynamic_cast<const Slider*>(node) != nullptr) slider=true;
    // Left/right still adjusts a selected slider. Up/down leaves it.
    if ((dx || dy) && !(slider && dx)) {
      directional = true;
      if (event.pressed) (void)m_inputDispatcher.moveSpatialFocus(dx,dy);
    }
  }
  if (!directional) m_inputDispatcher.keyEvent(event.sym, event.utf32, event.modifiers, event.pressed, event.preedit);
  if (m_surface != nullptr && m_sceneRoot != nullptr && (m_sceneRoot->paintDirty() || m_sceneRoot->layoutDirty())) {
    if (m_sceneRoot->layoutDirty()) {
      m_surface->requestLayout();
    } else {
      m_surface->requestRedraw();
    }
  }
}

bool PanelManager::attachedMorphEnabled() const {
  return (m_attachedToBar && m_islandMorph) || m_config==nullptr || m_config->config().shell.panel.attachedMorph;
}
float PanelManager::attachedAnimationDuration() const {
  if (!MotionService::instance().enabled()) return 0.0F;
  return m_config ? m_config->config().shell.panel.attachedDurationMs : 400.0F;
}
attached_panel::MorphGeometry PanelManager::attachedMorphGeometry(float progress) const {
  if (m_islandMorph && m_attachedSource.valid() && m_attachedPanelGeometry) {
    const auto& final=*m_attachedPanelGeometry;
    return attached_panel::islandMorphGeometry(
        {m_attachedSource.x-final.finalOutputX+m_panelInsetX,
         m_attachedSource.y-final.finalOutputY+m_panelInsetY,m_attachedSource.width,m_attachedSource.height},
        m_attachedSource.radii,
        {static_cast<float>(m_panelInsetX),static_cast<float>(m_panelInsetY),
         static_cast<float>(m_panelVisualWidth),static_cast<float>(m_panelVisualHeight)},
        m_activePanel?Style::scaledRadiusXl(m_activePanel->contentScale()):0.F,progress);
  }
  attached_panel::MorphSettings settings;
  if (m_config) {
    const auto& panel=m_config->config().shell.panel;
    settings={panel.attachedStartWidth,panel.attachedCornerGrowth,panel.attachedContentTravel};
  }
  return attached_panel::morphGeometry(
      {static_cast<float>(m_panelInsetX),static_cast<float>(m_panelInsetY),
       static_cast<float>(m_panelVisualWidth),static_cast<float>(m_panelVisualHeight)},
      m_activePanel ? Style::scaledRadiusXl(m_activePanel->contentScale()) : 0.F,
      m_attachedRevealDirection,progress,settings);
}

void PanelManager::applyAttachedMorph(float progress) {
  const auto geometry=attachedMorphGeometry(progress);
  const auto& bounds=geometry.background;
  const auto& clip=geometry.contentClip;
  const float width=m_sceneRoot->width(),height=m_sceneRoot->height();
  m_attachedRevealClipNode->setPosition(0,0);
  m_attachedRevealClipNode->setFrameSize(width,height);
  m_attachedRevealContentNode->setPosition(0,0);
  m_attachedRevealContentNode->setFrameSize(width,height);
  if (m_islandMorph && m_attachedSourceContentProvider && !m_islandOpenerProxy) {
    auto proxy=std::make_unique<RenderProxyNode>();
    proxy->setSourceProvider([this]() -> const Node* {
      if (!m_islandMorph || !m_attachedToBar || !m_attachedSourceContentProvider) return nullptr;
      return m_attachedSourceContentProvider(m_output,m_sourceBarName,m_attachedSource);
    });
    m_islandOpenerProxy=static_cast<RenderProxyNode*>(m_attachedRevealContentNode->addChild(std::move(proxy)));
  }
  if (m_islandOpenerProxy) {
    const auto* source=m_islandOpenerProxy->source();
    const bool show=m_islandMorph && source && m_attachedPanelGeometry && progress<1.F;
    m_islandOpenerProxy->setVisible(show);
    if (show) {
      const auto& final=*m_attachedPanelGeometry;
      const auto offset = m_attachedSource.contentOffset.value_or(AttachedPanelSource::ContentOffset{
          (m_attachedSource.width-source->width())*.5F,
          (m_attachedSource.height-source->height())*.5F});
      m_islandOpenerProxy->setPosition(m_attachedSource.x-final.finalOutputX+m_panelInsetX+offset.x,
          m_attachedSource.y-final.finalOutputY+m_panelInsetY+offset.y);
      m_islandOpenerProxy->setFrameSize(source->width(),source->height());
      m_islandOpenerProxy->setOpacity(1.F-std::clamp(progress,0.F,1.F));
    }
  }
  if (m_islandMorph && m_attachedSource.usageRing && !m_islandUsageRing) {
    auto ring = std::make_unique<CountdownRingNode>();
    ring->setHitTestVisible(false);
    ring->setParticipatesInLayout(false);
    ring->setZIndex(100);
    m_islandUsageRing = static_cast<CountdownRingNode*>(m_attachedRevealContentNode->addChild(std::move(ring)));
  }
  if (m_islandUsageRing) {
    m_islandUsageRing->setVisible(m_islandMorph && m_attachedSource.usageRing.has_value());
    if (m_attachedSource.usageRing) {
      auto style = *m_attachedSource.usageRing;
      const float inset = style.thickness * 2.5F;
      style.radius = std::max(0.0F, geometry.radius - inset);
      m_islandUsageRing->setStyle(style);
      m_islandUsageRing->setPosition(bounds.x + inset, bounds.y + inset);
      m_islandUsageRing->setFrameSize(std::max(0.0F, bounds.width - 2.0F * inset),
                                    std::max(0.0F, bounds.height - 2.0F * inset));
    }
  }
  applyAttachedDecorationStyle();
  if (m_bgNode) {
    auto* background=static_cast<Box*>(m_bgNode);
    background->setPosition(bounds.x,bounds.y);
    background->setFrameSize(bounds.width,bounds.height);
    background->setRadii(geometry.radii);
    background->setLogicalInset(m_islandMorph?RectInsets{}:attached_panel::logicalInset(m_attachedBarPosition,geometry.radius));
  }
  if (m_panelShadowNode && m_config) {
    const auto offset=shadowDirectionOffset(m_config->config().shell.shadow.direction);
    m_panelShadowNode->setPosition(bounds.x+offset.x,bounds.y+offset.y);
    m_panelShadowNode->setFrameSize(bounds.width,bounds.height);
    m_panelShadowNode->setOpacity(progress);
  }
  if (m_panelContactShadowNode) {
    const bool vertical=m_attachedRevealDirection==AttachedRevealDirection::Left || m_attachedRevealDirection==AttachedRevealDirection::Right;
    const float thickness=std::min(std::max(16.F*m_activePanel->contentScale(),geometry.radius*2),vertical?bounds.width:bounds.height);
    float x=bounds.x,y=bounds.y,w=bounds.width,h=bounds.height;
    if (vertical) {
      w=thickness;
      if (m_attachedRevealDirection==AttachedRevealDirection::Left) x+=bounds.width-w;
    } else {
      h=thickness;
      if (m_attachedRevealDirection==AttachedRevealDirection::Up) y+=bounds.height-h;
    }
    m_panelContactShadowNode->setPosition(x,y);
    m_panelContactShadowNode->setFrameSize(w,h);
    m_panelContactShadowNode->setOpacity(progress);
  }
  if (m_attachedContentClipNode && m_contentNode) {
    m_contentNode->setOpacity(m_islandMorph?std::clamp(progress,0.F,1.F):1.F);
    m_attachedContentClipNode->setPosition(clip.x,clip.y);
    m_attachedContentClipNode->setFrameSize(clip.width,clip.height);
    const float padding=m_activePanel && m_activePanel->hasDecoration()
        ? m_activePanel->contentScale()*Style::panelPadding : 0.F;
    m_contentNode->setPosition(m_panelInsetX+padding+geometry.contentX-clip.x,
        m_panelInsetY+padding+geometry.contentY-clip.y);
  }
  publishAttachedPanelGeometry(progress);
  // Surface::tessellateShape expands concave insets from the body itself. The
  // painted background already includes that expansion; passing it here would
  // expand the blur and input footprint a second time.
  applyPanelCompositorBlur(static_cast<int>(std::lround(geometry.body.x)),static_cast<int>(std::lround(geometry.body.y)),
      static_cast<int>(std::lround(geometry.body.width)),static_cast<int>(std::lround(geometry.body.height)),
      0,0,static_cast<int>(std::lround(width)),static_cast<int>(std::lround(height)),geometry.radius);
  if (m_panelOutputInputRect && !m_closing) {
    auto rect=*m_panelOutputInputRect;
    rect.x+=static_cast<int>(std::lround(geometry.body.x-m_panelInsetX));
    rect.y+=static_cast<int>(std::lround(geometry.body.y-m_panelInsetY));
    rect.width=static_cast<int>(std::lround(geometry.body.width));
    rect.height=static_cast<int>(std::lround(geometry.body.height));
    m_clickShield.setPanelInputRect(m_output,rect);
  }
}

void PanelManager::applyAttachedReveal(float progress) {
  m_attachedRevealProgress = std::clamp(progress, 0.0F, 1.0F);
  if (!m_attachedToBar || m_attachedRevealClipNode == nullptr || m_sceneRoot == nullptr) {
    if (m_attachedToBar && m_surface != nullptr) {
      m_surface->clearBlurRegion();
    }
    return;
  }

  if (attachedMorphEnabled()) {
    applyAttachedMorph(m_attachedRevealProgress);
    return;
  }
  if (m_islandOpenerProxy) m_islandOpenerProxy->setVisible(false);
  if (m_islandUsageRing) m_islandUsageRing->setVisible(false);
  if (m_contentNode) m_contentNode->setOpacity(1.F);
  if (m_attachedContentClipNode) {
    m_attachedContentClipNode->setPosition(0,0);
    m_attachedContentClipNode->setFrameSize(m_sceneRoot->width(),m_sceneRoot->height());
    if (m_contentNode && m_activePanel) {
      const float padding=m_activePanel->hasDecoration()?m_activePanel->contentScale()*Style::panelPadding:0.F;
      m_contentNode->setPosition(m_panelInsetX+padding,m_panelInsetY+padding);
    }
  }

  const float w = m_sceneRoot->width();
  const float h = m_sceneRoot->height();
  const float panelW = m_panelVisualWidth > 0 ? static_cast<float>(m_panelVisualWidth) : w;
  const float panelH = m_panelVisualHeight > 0 ? static_cast<float>(m_panelVisualHeight) : h;
  const float travelX = (m_attachedRevealDirection == AttachedRevealDirection::Left
                         || m_attachedRevealDirection == AttachedRevealDirection::Right)
      ? panelW * (1.0F - m_attachedRevealProgress)
      : 0.0F;
  const float travelY = (m_attachedRevealDirection == AttachedRevealDirection::Up
                         || m_attachedRevealDirection == AttachedRevealDirection::Down)
      ? panelH * (1.0F - m_attachedRevealProgress)
      : 0.0F;

  float contentX = 0.0F;
  float contentY = 0.0F;
  switch (m_attachedRevealDirection) {
  case AttachedRevealDirection::Down:
    contentY = -travelY;
    break;
  case AttachedRevealDirection::Up:
    contentY = travelY;
    break;
  case AttachedRevealDirection::Right:
    contentX = -travelX;
    break;
  case AttachedRevealDirection::Left:
    contentX = travelX;
    break;
  }

  m_attachedRevealClipNode->setPosition(0.0F, 0.0F);
  m_attachedRevealClipNode->setFrameSize(w, h);

  if (m_attachedRevealContentNode != nullptr) {
    m_attachedRevealContentNode->setPosition(contentX, contentY);
    m_attachedRevealContentNode->setFrameSize(w, h);
  }
  if (m_panelShadowNode != nullptr) {
    m_panelShadowNode->setOpacity(m_attachedRevealProgress);
  }
  if (m_panelContactShadowNode != nullptr) {
    m_panelContactShadowNode->setOpacity(m_attachedRevealProgress);
  }

  publishAttachedPanelGeometry(m_attachedRevealProgress);
  const int bodyX = m_panelInsetX + static_cast<int>(std::lround(contentX));
  const int bodyY = m_panelInsetY + static_cast<int>(std::lround(contentY));
  applyPanelCompositorBlur(
      bodyX, bodyY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight), 0, 0,
      static_cast<int>(std::lround(w)), static_cast<int>(std::lround(h))
  );
}

void PanelManager::applyDetachedReveal(float progress) {
  m_detachedRevealProgress = std::clamp(progress, 0.0F, 1.0F);
  if (m_attachedToBar || m_sceneRoot == nullptr) {
    if (!m_attachedToBar && m_surface != nullptr) {
      m_surface->clearBlurRegion();
    }
    return;
  }

  const float surfaceW = m_sceneRoot->width();
  const float surfaceH = m_sceneRoot->height();
  float clipX = 0.0F;
  float clipY = 0.0F;
  float clipW = surfaceW;
  float clipH = surfaceH;

  switch (m_detachedRevealDirection) {
  case AttachedRevealDirection::Down:
    clipH = std::round(surfaceH * m_detachedRevealProgress);
    break;
  case AttachedRevealDirection::Up:
    clipH = std::round(surfaceH * m_detachedRevealProgress);
    clipY = surfaceH - clipH;
    break;
  case AttachedRevealDirection::Right:
    clipW = std::round(surfaceW * m_detachedRevealProgress);
    break;
  case AttachedRevealDirection::Left:
    clipW = std::round(surfaceW * m_detachedRevealProgress);
    clipX = surfaceW - clipW;
    break;
  }

  if (m_detachedRevealClipNode != nullptr && m_detachedRevealContentNode != nullptr) {
    m_detachedRevealClipNode->setPosition(clipX, clipY);
    m_detachedRevealClipNode->setFrameSize(clipW, clipH);
    m_detachedRevealContentNode->setPosition(-clipX, -clipY);
    m_detachedRevealContentNode->setFrameSize(surfaceW, surfaceH);
  }

  if (m_contentNode != nullptr) {
    m_contentNode->setOpacity(panelRevealContentOpacity(m_detachedRevealProgress));
  }
  applyPanelCompositorBlur(
      m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight),
      static_cast<int>(std::lround(clipX)), static_cast<int>(std::lround(clipY)), static_cast<int>(std::lround(clipW)),
      static_cast<int>(std::lround(clipH))
  );
}

void PanelManager::startAttachedOpenAnimation() {
  if (!m_attachedOpenAnimationPending || !m_attachedToBar || m_attachedRevealClipNode == nullptr || m_closing) {
    return;
  }
  if (attachedAnimationDuration()<=0.0F) {
    m_attachedOpenAnimationPending=false;
    applyAttachedReveal(1.0F);
    return;
  }
  if (m_attachedPanelBarSettledCallback != nullptr
      && m_output != nullptr
      && !m_attachedPanelBarSettledCallback(m_output, m_sourceBarName)) {
    return;
  }

  m_attachedOpenAnimationPending = false;
  m_animations.animate(
      m_attachedRevealProgress, 1.0F, attachedAnimationDuration(), Easing::EaseOutCubic,
      [this](float v) { applyAttachedReveal(v); }, {}, m_attachedRevealClipNode
  );
}

void PanelManager::publishAttachedPanelGeometry(float revealProgress) {
  if (!m_attachedToBar || !m_attachedPanelGeometryCallback || m_output == nullptr || !m_attachedPanelGeometry) {
    return;
  }

  const float progress = std::clamp(revealProgress, 0.0F, 1.0F);
  if (progress <= 0.001F) {
    m_attachedPanelGeometryCallback(m_output, m_sourceBarName, std::nullopt);
    return;
  }

  auto geometry = *m_attachedPanelGeometry;
  const float baseLocalX = geometry.x;
  const float baseLocalY = geometry.y;
  geometry.cornerPower = m_sceneRoot ? m_sceneRoot->cornerPower() : Style::cornerPower;
  geometry.revealProgress = progress;
  const auto syncOutputGeometry = [&]() {
    geometry.outputX += geometry.x - baseLocalX;
    geometry.outputY += geometry.y - baseLocalY;
    geometry.outputWidth = geometry.width;
    geometry.outputHeight = geometry.height;
  };

  if (attachedMorphEnabled()) {
    const bool vertical=m_attachedRevealDirection==AttachedRevealDirection::Left
        || m_attachedRevealDirection==AttachedRevealDirection::Right;
    const auto frame=attachedMorphGeometry(progress);
    const float fullRadius=geometry.cornerRadius;
    const float baseX=geometry.x+(vertical || m_islandMorph?0.F:fullRadius);
    const float baseY=geometry.y+(vertical && !m_islandMorph?fullRadius:0.F);
    geometry.x=baseX+frame.background.x-m_panelInsetX;
    geometry.y=baseY+frame.background.y-m_panelInsetY;
    geometry.width=frame.background.width;geometry.height=frame.background.height;
    geometry.cornerRadius=frame.radius;geometry.bulgeRadius=m_islandMorph?0.F:frame.radius;
    geometry.panelOwnsSource=m_islandMorph;
    syncOutputGeometry();
    m_attachedPanelGeometryCallback(m_output,m_sourceBarName,geometry);
    return;
  }

  // The bar-side concave bulges only enter the visible clip during the last
  // portion of the animation. Until then the silhouette is a sharp-edged rectangle.
  const float originalRadius = geometry.cornerRadius;
  const bool vertical =
      (m_attachedRevealDirection == AttachedRevealDirection::Right
       || m_attachedRevealDirection == AttachedRevealDirection::Left);
  const float panelMainDim = vertical ? geometry.width : geometry.height;
  const float bulgeRevealAmount = std::clamp(originalRadius - panelMainDim * (1.0F - progress), 0.0F, originalRadius);
  const float crossDelta = originalRadius - bulgeRevealAmount;
  geometry.bulgeRadius = bulgeRevealAmount;

  // The away-side convex corners are visible at full radius throughout the animation.
  // Extend the body main-axis dimension toward the bar so it is at least 2*cornerRadius.
  const float minMainDim = 2.0F * originalRadius;

  switch (m_attachedRevealDirection) {
  case AttachedRevealDirection::Down: {
    const float visibleHeight = geometry.height * progress;
    const float effectiveHeight = std::max(visibleHeight, minMainDim);
    const float extension = effectiveHeight - visibleHeight;
    geometry.y -= extension;
    geometry.height = effectiveHeight;
    geometry.x += crossDelta;
    geometry.width -= 2.0F * crossDelta;
    break;
  }
  case AttachedRevealDirection::Up: {
    const float originalHeight = geometry.height;
    const float visibleHeight = originalHeight * progress;
    const float effectiveHeight = std::max(visibleHeight, minMainDim);
    geometry.y += originalHeight - visibleHeight;
    geometry.height = effectiveHeight;
    geometry.x += crossDelta;
    geometry.width -= 2.0F * crossDelta;
    break;
  }
  case AttachedRevealDirection::Right: {
    const float visibleWidth = geometry.width * progress;
    const float effectiveWidth = std::max(visibleWidth, minMainDim);
    const float extension = effectiveWidth - visibleWidth;
    geometry.x -= extension;
    geometry.width = effectiveWidth;
    geometry.y += crossDelta;
    geometry.height -= 2.0F * crossDelta;
    break;
  }
  case AttachedRevealDirection::Left: {
    const float originalWidth = geometry.width;
    const float visibleWidth = originalWidth * progress;
    const float effectiveWidth = std::max(visibleWidth, minMainDim);
    geometry.x += originalWidth - visibleWidth;
    geometry.width = effectiveWidth;
    geometry.y += crossDelta;
    geometry.height -= 2.0F * crossDelta;
    break;
  }
  }

  syncOutputGeometry();
  m_attachedPanelGeometryCallback(m_output, m_sourceBarName, geometry);
}

void PanelManager::applyPanelCompositorBlur(
    int bodyX, int bodyY, int bodyW, int bodyH, int clipX, int clipY, int clipW, int clipH, float shapeRadius
) {
  // The blur region is compositor surface state, not a scene node. Callers pass the
  // same body and clip rectangles used by the reveal animation so protocol state
  // cannot get ahead of scene rendering.
  if (m_surface == nullptr || m_activePanel == nullptr) {
    return;
  }

  if (blurTraceEnabled()) {
    kLog.debug(
        "blur-trace panel-blur-input mode={} progress={:.3F} phase={} surface={}x{} body={}:{}+{}x{} "
        "clip={}:{}+{}x{}",
        m_attachedToBar ? "attached" : "detached",
        m_attachedToBar ? m_attachedRevealProgress : m_detachedRevealProgress, uiPhaseName(currentUiPhase()),
        m_surface->width(), m_surface->height(), bodyX, bodyY, bodyW, bodyH, clipX, clipY, clipW, clipH
    );
  }

  if (bodyW <= 0 || bodyH <= 0 || clipW <= 0 || clipH <= 0) {
    if (blurTraceEnabled()) {
      kLog.debug("blur-trace panel-blur-clear reason=empty-input");
    }
    m_surface->clearBlurRegion();
    if (m_attachedToBar) m_surface->setInputRegion({});
    return;
  }

  const float radius = shapeRadius>=0 ? shapeRadius : Style::scaledRadiusXl(m_activePanel->contentScale());
  const CornerShapes corners = m_attachedToBar && !m_islandMorph ? attached_panel::cornerShapes(m_attachedBarPosition) : CornerShapes{};
  const RectInsets logicalInset =
      m_attachedToBar && !m_islandMorph ? attached_panel::logicalInset(m_attachedBarPosition, radius) : RectInsets{};
  const Radii radii = m_attachedToBar && m_islandMorph ? attachedMorphGeometry(m_attachedRevealProgress).radii
      : Radii{radius, radius, radius, radius};
  auto strips = Surface::tessellateShape(bodyX, bodyY, bodyW, bodyH, corners, logicalInset, radii, 1, m_sceneRoot ? m_sceneRoot->cornerPower() : Style::cornerPower);
  if (strips.empty()) {
    if (blurTraceEnabled()) {
      kLog.debug("blur-trace panel-blur-clear reason=empty-shape");
    }
    m_surface->clearBlurRegion();
    if (m_attachedToBar) m_surface->setInputRegion({});
    return;
  }

  const int clipRight = clipX + clipW;
  const int clipBottom = clipY + clipH;
  std::vector<InputRect> clipped;
  clipped.reserve(strips.size());
  for (const auto& s : strips) {
    const int sxLeft = std::max(s.x, clipX);
    const int sxRight = std::min(s.x + s.width, clipRight);
    const int syTop = std::max(s.y, clipY);
    const int syBottom = std::min(s.y + s.height, clipBottom);
    if (sxRight > sxLeft && syBottom > syTop) {
      clipped.push_back({sxLeft, syTop, sxRight - sxLeft, syBottom - syTop});
    }
  }

  if (clipped.empty()) {
    if (blurTraceEnabled()) {
      kLog.debug("blur-trace panel-blur-clear reason=empty-clipped");
    }
    m_surface->clearBlurRegion();
    if (m_attachedToBar) m_surface->setInputRegion({});
    return;
  }

  if (blurTraceEnabled()) {
    const auto bounds = boundsForPanelTrace(clipped);
    kLog.debug(
        "blur-trace panel-blur-set mode={} progress={:.3F} rects={} bounds={}:{}+{}x{}",
        m_attachedToBar ? "attached" : "detached",
        m_attachedToBar ? m_attachedRevealProgress : m_detachedRevealProgress, clipped.size(), bounds.x, bounds.y,
        bounds.width, bounds.height
    );
  }
  m_surface->setBlurRegion(clipped);
  if (m_attachedToBar) m_surface->setInputRegion(m_closing?std::vector<InputRect>{}:clipped);
}

void PanelManager::applyAttachedDecorationStyle() {
  if (!m_attachedToBar || m_activePanel == nullptr) {
    return;
  }
  const float scale = m_activePanel->contentScale();
  const float radius = attachedMorphEnabled() ? attachedMorphGeometry(m_attachedRevealProgress).radius : Style::scaledRadiusXl(scale);
  const auto corners=m_islandMorph?CornerShapes{}:attached_panel::cornerShapes(m_attachedBarPosition);
  const auto inset=m_islandMorph?RectInsets{}:attached_panel::logicalInset(m_attachedBarPosition,radius);
  const auto radii=m_islandMorph?attachedMorphGeometry(m_attachedRevealProgress).radii:Radii{radius,radius,radius,radius};

  if (m_bgNode != nullptr) {
    auto* bg = static_cast<Box*>(m_bgNode);
    bg->setFill(scaleAlpha(m_attachedBackground, m_attachedBackgroundOpacity));
    bg->setCornerShapes(corners);
    bg->setLogicalInset(inset);
    bg->setRadii(radii);
  }

  if (m_panelShadowNode != nullptr && m_config != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    m_panelShadowNode->setVisible(panelShadow);
    if (panelShadow) {
      const RoundedRectStyle shadowStyle = shell::surface_shadow::style(
          shadowConfig, m_attachedBackgroundOpacity,
          shell::surface_shadow::Shape{
              .corners = corners,
              .logicalInset = inset,
              .radius = radii,
          }
      );
      m_panelShadowNode->setStyle(shadowStyle);
    }
  }

  if (m_panelContactShadowNode != nullptr) {
    m_panelContactShadowNode->setVisible(m_attachedContactShadow && !m_islandMorph);
    const float contactAlpha = 0.16F * std::clamp(m_attachedBackgroundOpacity, 0.0F, 1.0F);
    const bool barIsBottom = m_attachedBarPosition == "bottom";
    const bool barIsRight = m_attachedBarPosition == "right";
    const bool barIsVertical = m_attachedBarPosition == "left" || m_attachedBarPosition == "right";
    // Gradient runs perpendicular to the bar edge, dark next to the bar, transparent toward
    // the panel interior. For top/left: dark at start. For bottom/right: dark at end.
    const bool darkAtStart = !(barIsBottom || barIsRight);
    const Color darkColor = rgba(0.0F, 0.0F, 0.0F, contactAlpha);
    const Color clearGradient = rgba(0.0F, 0.0F, 0.0F, 0.0F);
    const Color startColor = darkAtStart ? darkColor : clearGradient;
    const Color endColor = darkAtStart ? clearGradient : darkColor;
    const RoundedRectStyle contactStyle{
        .fill = startColor,
        .border = clearColor(),
        .fillMode = FillMode::LinearGradient,
        .gradientDirection = barIsVertical ? GradientDirection::Horizontal : GradientDirection::Vertical,
        .gradientStops =
            {GradientStop{0.0F, startColor}, GradientStop{0.0F, startColor}, GradientStop{1.0F, endColor},
             GradientStop{1.0F, endColor}},
        .corners = attached_panel::cornerShapes(m_attachedBarPosition),
        .logicalInset = attached_panel::logicalInset(m_attachedBarPosition, radius),
        .radius = Radii{radius, radius, radius, radius},
        .softness = 1.0F,
        .borderWidth = 0.0F,
    };
    m_panelContactShadowNode->setStyle(contactStyle);
  }
}

void PanelManager::onSurfaceConfigured() {
  if (m_attachedPlacement) {
    m_attachedAwaitingConfigure = false;
    m_attachedPlacementPending = true;
  }
  m_sceneGeometryDirty = true;
  if (m_surface) m_surface->requestLayout();
}

void PanelManager::refreshPanelPlacement() {
  if (m_refreshingRetainedPlacement || !isOpen() || m_closing || !m_config || !m_platform || !m_layerSurface || !m_surface)
    return;
  struct Reset { bool& flag; ~Reset() { flag = false; } } reset{m_refreshingRetainedPlacement};
  m_refreshingRetainedPlacement = true;
  const auto barConfigOpt = resolvePanelBarConfig(m_config, m_platform, m_output, m_openingSourceBarName);
  auto bar = barConfigOpt.value_or(BarConfig{});
  const auto* output = m_platform->findOutputByWl(m_output);
  if (!output) return;
  const int outputWidth = output->effectiveLogicalWidth(), outputHeight = output->effectiveLogicalHeight();
  if (outputWidth <= 0 || outputHeight <= 0) return;
  const bool wantsAttachment = m_activePanel->panelPlacement() == PanelPlacement::Attached;
  bool available = barConfigOpt && !m_config->config().bars.empty() && bar.enabled && bar.thickness > 0;
  if (available && m_attachedPanelAvailabilityCallback) {
    const auto generation = m_destroyGeneration;
    available = m_attachedPanelAvailabilityCallback(m_output, bar.name);
    if (generation != m_destroyGeneration || !isOpen()) return;
  }
  if (!wantsAttachment || !available) {
    const auto& panelConfig = m_config->config().shell.panel;
    const std::string position = m_activePanelId.contains(':') ? m_activePanel->panelScreenPosition()
                                                              : resolvePanelPosition(m_config, m_activePanelId);
    const bool centered = position == "center" || (!available && (wantsAttachment || position == "auto"));
    const bool automatic = !centered && position == "auto";
    const int pad = std::max(0, static_cast<int>(Style::spaceSm));
    const int bodyW = std::clamp(static_cast<int>(std::lround(preferredPanelWidth())),1,std::max(1,outputWidth-2*pad));
    const int bodyH = std::clamp(static_cast<int>(std::lround(preferredPanelHeight())),1,std::max(1,outputHeight-2*pad));
    int x=(outputWidth-bodyW)/2, y=(outputHeight-bodyH)/2;
    bool right=false, bottom=false;
    if (automatic) {
      const auto rect=resolveBarVisibleRect(bar,outputWidth,outputHeight);
      const bool vertical=bar.position=="left" || bar.position=="right";
      right=bar.position=="right"; bottom=bar.position=="bottom";
      const bool near=m_attachedAnchorAvailable && openNearClickEnabled(m_activePanel,m_activePanelId,m_config);
      if (vertical) {
        x=right?rect.left-bodyW-panelConfig.floatingOffset:rect.right+panelConfig.floatingOffset;
        y=near?static_cast<int>(std::lround(m_attachedAnchorY-bodyH*.5F)):(rect.top+rect.bottom-bodyH)/2;
      } else {
        y=bottom?rect.top-bodyH-panelConfig.floatingOffset:rect.bottom+panelConfig.floatingOffset;
        x=near?static_cast<int>(std::lround(m_attachedAnchorX-bodyW*.5F)):(rect.left+rect.right-bodyW)/2;
      }
      x=std::clamp(x,std::min(pad,outputWidth-bodyW),std::max(pad,outputWidth-bodyW-pad));
      y=std::clamp(y,std::min(pad,outputHeight-bodyH),std::max(pad,outputHeight-bodyH-pad));
    } else if (!centered) {
      const auto edge=shell::screenPositionAnchor(position,panelConfig.floatingOffset);
      right=(edge.anchor&LayerShellAnchor::Right)!=0; bottom=(edge.anchor&LayerShellAnchor::Bottom)!=0;
      if (right) x=outputWidth-bodyW-edge.marginRight;
      else if (edge.anchor&LayerShellAnchor::Left) x=edge.marginLeft;
      if (bottom) y=outputHeight-bodyH-edge.marginBottom;
      else if (edge.anchor&LayerShellAnchor::Top) y=edge.marginTop;
      x=std::clamp(x,0,outputWidth-bodyW); y=std::clamp(y,0,outputHeight-bodyH);
    }
    const auto bleed=shell::panel_surface::bleed(m_activePanel->hasDecoration(),m_config->config().shell.shadow);
    RetainedPlacement placement{.attached=false,.anchoredRight=right,.anchoredBottom=bottom,
        .fillWidth=m_activePanel->fillsWidth() && !wantsAttachment,
        .fillHeight=m_activePanel->fillsHeight() && !wantsAttachment,
        .barName=available?bar.name:std::string{},.position=position,.body={x,y,bodyW,bodyH},
        .insetX=bleed.left,.insetY=bleed.up,.trailingX=bleed.right,.trailingY=bleed.down};
    if (placement.fillWidth) {placement.body.x=pad;placement.body.width=std::max(1,outputWidth-2*pad);placement.anchoredRight=false;}
    if (placement.fillHeight) {placement.body.y=pad;placement.body.height=std::max(1,outputHeight-2*pad);placement.anchoredBottom=false;}
    placement.surfaceWidth=shell::panel_surface::surfaceExtent(placement.body.width,bleed.left,bleed.right);
    placement.surfaceHeight=shell::panel_surface::surfaceExtent(placement.body.height,bleed.up,bleed.down);
    placement.layer=resolveFloatingPanelLayer(m_config,m_activePanelId,*m_activePanel);
    placement.opacity=shell::panel_surface::backgroundOpacity(m_config);
    placement.detachedDirection=detachedRevealDirection(centered?"center":position,bar.position);
    auto anchor=(bottom?LayerShellAnchor::Bottom:LayerShellAnchor::Top)|(right?LayerShellAnchor::Right:LayerShellAnchor::Left);
    const int sx=placement.body.x-bleed.left,sy=placement.body.y-bleed.up;
    const int reserved=automatic && bar.reserveSpace?reservedBarEdgeDistance(bar,m_config->config().shell.shadow):0;
    int top=bottom?0:sy-((automatic && bar.position=="top")?reserved:0);
    int end=right?outputWidth-static_cast<int>(placement.surfaceWidth)-sx-reserved:0;
    int foot=bottom?outputHeight-static_cast<int>(placement.surfaceHeight)-sy-reserved:0;
    int start=right?0:sx-((automatic && bar.position=="left")?reserved:0);
    if (placement.fillWidth) {anchor|=LayerShellAnchor::Left|LayerShellAnchor::Right;start=pad-bleed.left;end=pad-bleed.right;}
    if (placement.fillHeight) {anchor|=LayerShellAnchor::Top|LayerShellAnchor::Bottom;top=pad-bleed.up;foot=pad-bleed.down;}
    const int exclusiveZone = centered && !placement.fillWidth && !placement.fillHeight ? -1 : 0;
    queuePanelPlacement(std::move(placement),anchor,top,end,foot,start,exclusiveZone);
    return;
  }
  if (m_attachedPanelLayerProvider) {
    const auto generation = m_destroyGeneration;
    const auto layer = m_attachedPanelLayerProvider(m_output, bar.name);
    if (generation != m_destroyGeneration || !isOpen()) return;
    if (layer) bar.layer = *layer;
  }
  const auto rect = resolveBarVisibleRect(bar, outputWidth, outputHeight);
  const bool left = bar.position == "left", right = bar.position == "right";
  const bool bottom = bar.position == "bottom", vertical = left || right;
  const auto direction = attached_panel::revealDirection(bar.position);
  const auto& shadow = m_config->config().shell.shadow;
  const auto bleed = shell::surface_shadow::bleed(m_activePanel->hasDecoration(), shadow, "panel", "panel");
  const float radius = Style::scaledRadiusXl(m_activePanel->contentScale());
  const int startRadius = vertical ? (left ? bar.radiusTopRight : bar.radiusTopLeft)
                                  : (bottom ? bar.radiusTopLeft : bar.radiusBottomLeft);
  const int endRadius = vertical ? (left ? bar.radiusBottomRight : bar.radiusBottomLeft)
                                : (bottom ? bar.radiusTopRight : bar.radiusBottomRight);
  m_attachedHasAnchor = m_attachedAnchorAvailable && openNearClickEnabled(m_activePanel, m_activePanelId, m_config);
  auto body = attached_panel::fitBody(direction,
      {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top}, outputWidth, outputHeight,
      static_cast<int>(std::lround(preferredPanelWidth())),
      static_cast<int>(std::lround(preferredPanelHeight())), static_cast<int>(Style::spaceSm),
      bar.panelOverlap, static_cast<int>(std::ceil(startRadius + radius)),
      static_cast<int>(std::ceil(endRadius + radius)),
      m_attachedHasAnchor ? std::optional<float>(vertical ? m_attachedAnchorY : m_attachedAnchorX) : std::nullopt);
  if (bar.sectionBackgrounds && bar.islandMorph && m_attachedSourceGeometryProvider) {
    const auto generation = m_destroyGeneration;
    const auto source = m_attachedSourceGeometryProvider(m_output, bar.name, m_attachedSource);
    if (generation != m_destroyGeneration || !isOpen()) return;
    if (!source && !m_attachedSource.sectionId.empty()) {
      // Configured sections use stable identities. Once that identity is
      // removed there is no safe node to proxy or close animation to.
      closePanel(false);
      return;
    }
    // A temporarily unconfigured bar keeps the last valid close target.
    if (source && source->valid()) m_attachedSource = *source;
  }
  const bool islandMorph=bar.sectionBackgrounds && bar.islandMorph && m_attachedSource.valid();
  if (islandMorph) {
    body = bar.islandOutward ? attached_panel::fitOutwardIsland(direction,
        {rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top}, m_attachedSource,
        outputWidth, outputHeight, static_cast<int>(std::lround(preferredPanelWidth())),
        static_cast<int>(std::lround(preferredPanelHeight())), static_cast<int>(Style::spaceSm), bar.islandPanelOffset)
        : attached_panel::includeIslandSource(body, m_attachedSource, outputWidth, outputHeight);
  }
  const int cross = static_cast<int>(std::ceil(radius)) + 2 +
      (vertical ? std::max(bleed.up, bleed.down) : std::max(bleed.left, bleed.right));
  const int away = 2 + (vertical ? (left ? bleed.right : bleed.left) : (bottom ? bleed.up : bleed.down));
  const int toward=islandMorph?2+(vertical?(left?bleed.left:bleed.right):(bottom?bleed.down:bleed.up)):0;
  RetainedPlacement placement{
      .islandMorph = islandMorph,
      .anchoredRight = right, .anchoredBottom = bottom,
      .barName = bar.name, .position = bar.position, .body = body,
      .insetX = vertical ? (left ? toward : away) : cross,
      .insetY = vertical ? cross : (bottom ? away : toward),
      .trailingX = vertical ? (left ? away : toward) : cross,
      .trailingY = vertical ? cross : (bottom ? toward : away)};
  placement.surfaceWidth = shell::panel_surface::surfaceExtent(body.width, placement.insetX, placement.trailingX);
  placement.surfaceHeight = shell::panel_surface::surfaceExtent(body.height, placement.insetY, placement.trailingY);
  const auto barBleed = shell::surface_shadow::bleed(bar.shadow, shadow, "bar", "bar");
  const auto concave = barConcaveShape(bar);
  const int start = static_cast<int>(std::ceil(std::max(0.F, vertical ? concave.logicalInset.top : concave.logicalInset.left)));
  const int ends = std::max(0, bar.marginEnds);
  placement.barOriginX = vertical ? (left ? std::max(0, rect.left - barBleed.left) : rect.left - barBleed.left)
                                 : rect.left - std::min(ends, barBleed.left + start);
  placement.barOriginY = vertical ? rect.top - std::min(ends, barBleed.up + start)
                                 : (bottom ? rect.top - barBleed.up : std::max(0, rect.top - barBleed.up));
  placement.layer = layerShellLayerFromConfig(bar.layer);
  placement.background = bar.background;
  placement.opacity = m_activePanel->inheritsBarBackgroundOpacity() ? bar.backgroundOpacity
                                                                  : m_activePanel->attachedBackgroundOpacityOverride();
  placement.contactShadow = bar.contactShadow;
  const int surfaceX = body.x - placement.insetX, surfaceY = body.y - placement.insetY;
  const int reserved = bar.reserveSpace ? reservedBarEdgeDistance(bar, shadow) : 0;
  const auto anchor = (bottom ? LayerShellAnchor::Bottom : LayerShellAnchor::Top) |
                      (right ? LayerShellAnchor::Right : LayerShellAnchor::Left);
  const int width=static_cast<int>(placement.surfaceWidth),height=static_cast<int>(placement.surfaceHeight);
  queuePanelPlacement(std::move(placement),anchor,
      bottom ? 0 : surfaceY - (vertical ? 0 : reserved),
      right ? outputWidth - width - surfaceX - reserved : 0,
      bottom ? outputHeight - height - surfaceY - reserved : 0,
      right ? 0 : surfaceX - (left ? reserved : 0),0);
}

void PanelManager::queuePanelPlacement(RetainedPlacement placement, std::uint32_t anchor,
                                     int top,int right,int bottom,int left,int exclusiveZone) {
  const bool sizeRequestChanged = !m_attachedPlacement ||
      placement.surfaceWidth != m_attachedPlacement->surfaceWidth || placement.surfaceHeight != m_attachedPlacement->surfaceHeight ||
      placement.fillWidth != m_attachedPlacement->fillWidth || placement.fillHeight != m_attachedPlacement->fillHeight;
  if (sizeRequestChanged && (placement.surfaceWidth != m_surface->width() || placement.surfaceHeight != m_surface->height()))
    m_attachedAwaitingConfigure = true;
  placement.request = PlacementRequest{anchor,top,right,bottom,left,exclusiveZone};
  const bool protocolChanged = sizeRequestChanged || !m_attachedPlacement ||
      placement.request != m_attachedPlacement->request;
  ++m_attachedPlacementGeneration;
  m_attachedPlacement = placement;
  m_attachedPlacementPending = true;
  // A fill request ends with zero on its stretched axis. Do not reissue the
  // preliminary bounded extent on unrelated reloads and trigger another configure.
  if (protocolChanged) {
    m_layerSurface->setExclusiveZone(exclusiveZone);
    m_layerSurface->requestPlacement(anchor, placement.surfaceWidth, placement.surfaceHeight,top,right,bottom,left);
    if (placement.fillWidth || placement.fillHeight)
      m_layerSurface->requestSize(placement.fillWidth?0:placement.surfaceWidth,placement.fillHeight?0:placement.surfaceHeight);
  }
  m_surface->requestLayout();
}

void PanelManager::applyAttachmentMode(bool attached) {
  if (!attached) m_islandMorph=false;
  if (!attached && m_islandOpenerProxy) m_islandOpenerProxy->setVisible(false);
  if (!attached && m_islandUsageRing) m_islandUsageRing->setVisible(false);
  if (m_attachedToBar == attached) return;
  Node* revealClip=m_attachedToBar?m_attachedRevealClipNode:m_detachedRevealClipNode;
  Node* revealContent=m_attachedToBar?m_attachedRevealContentNode:m_detachedRevealContentNode;
  if (revealClip) m_animations.cancelForOwner(revealClip);
  if (m_sceneRoot) m_animations.cancelForOwner(m_sceneRoot.get());
  m_attachedOpenAnimationPending=false;
  m_attachedRevealProgress=m_detachedRevealProgress=1.F;
  m_attachedToBar=attached;
  if (m_bgNode) m_bgNode->setOpacity(1.F);
  if (m_panelShadowNode) m_panelShadowNode->setOpacity(1.F);
  if (attached) {
    m_attachedRevealClipNode=revealClip;m_attachedRevealContentNode=revealContent;
    m_detachedRevealClipNode=m_detachedRevealContentNode=nullptr;
    if (!m_attachedContentClipNode && revealContent && m_contentNode) {
      auto retained=revealContent->removeChild(m_contentNode);
      auto clip=std::make_unique<Node>(); clip->setClipChildren(true);
      m_attachedContentClipNode=revealContent->addChild(std::move(clip));
      m_attachedContentClipNode->addChild(std::move(retained));
    }
    if (m_attachedContentClipNode) m_attachedContentClipNode->setClipChildren(true);
  } else {
    m_detachedRevealClipNode=revealClip;m_detachedRevealContentNode=revealContent;
    m_attachedRevealClipNode=m_attachedRevealContentNode=nullptr;
    if (m_attachedContentClipNode) m_attachedContentClipNode->setClipChildren(false);
    m_attachedPanelGeometry.reset();
    if (m_panelContactShadowNode) m_panelContactShadowNode->setVisible(false);
    if (m_bgNode) {
      auto* bg=static_cast<Box*>(m_bgNode);
      bg->setCornerShapes({});bg->setLogicalInset({});bg->setPanelStyle(m_config && m_config->config().shell.panel.borders);
    }
  }
}

void PanelManager::applyPanelPlacement(std::uint32_t width, std::uint32_t height) {
  if (!m_attachedPlacement || m_attachedAwaitingConfigure || !m_activePanel) return;
  const auto placement = *m_attachedPlacement;
  const auto placementGeneration = m_attachedPlacementGeneration;
  const bool right = placement.anchoredRight, bottom = placement.anchoredBottom;
  const bool vertical = right || placement.position == "left";
  const auto accepted = attached_panel::configuredBody(placement.body, placement.insetX, placement.insetY,
      placement.trailingX, placement.trailingY, placement.surfaceWidth, placement.surfaceHeight, width, height, right, bottom);
  if (m_attachedToBar && (!placement.attached || m_sourceBarName != placement.barName) && m_attachedPanelGeometryCallback && m_output) {
    const auto generation = m_destroyGeneration;
    m_attachedPanelGeometryCallback(m_output, m_sourceBarName, std::nullopt);
    if (generation != m_destroyGeneration || placementGeneration != m_attachedPlacementGeneration || !m_attachedToBar) return;
  }
  applyAttachmentMode(placement.attached);
  m_islandMorph=placement.attached && placement.islandMorph;
  m_sourceBarName = placement.barName;
  m_attachedBarPosition = placement.attached ? placement.position : std::string{};
  m_attachedRevealDirection = attached_panel::revealDirection(placement.position);
  if (!placement.attached) m_detachedRevealDirection=placement.detachedDirection;
  m_panelInsetX = accepted.insetX; m_panelInsetY = accepted.insetY;
  m_attachedBleedRight = accepted.trailingX; m_attachedBleedBottom = accepted.trailingY;
  m_detachedBleedRight = accepted.trailingX; m_detachedBleedBottom = accepted.trailingY;
  m_panelFillWidth=placement.fillWidth;m_panelFillHeight=placement.fillHeight;
  m_panelVisualWidth = accepted.outputBody.width; m_panelVisualHeight = accepted.outputBody.height;
  m_attachedBarOriginX = placement.barOriginX; m_attachedBarOriginY = placement.barOriginY;
  m_attachedBackground = placement.background;
  m_attachedBackgroundOpacity = placement.opacity;
  m_attachedContactShadow = placement.contactShadow;
  m_activePanel->setPanelCardOpacity(shell::panel_surface::cardOpacity(m_config, placement.opacity));
  if (placement.layer != m_panelLayer) {
    m_clickShield.setLayer(placement.layer);
    if (m_layerSurface) m_layerSurface->setLayer(placement.layer);
    m_panelLayer = placement.layer;
  }
  const auto& body = accepted.outputBody;
  if (m_panelOutputInputRect) {
    m_panelOutputInputRect = InputRect{body.x, body.y, body.width, body.height};
    m_clickShield.setPanelInputRect(m_output, *m_panelOutputInputRect);
  }
  const float radius = Style::scaledRadiusXl(m_activePanel->contentScale());
  if (placement.attached) m_attachedPanelGeometry = AttachedPanelGeometry{
      .x = static_cast<float>(body.x - placement.barOriginX) - (vertical ? 0.F : radius),
      .y = static_cast<float>(body.y - placement.barOriginY) - (vertical ? radius : 0.F),
      .width = body.width + (vertical ? 0.F : radius * 2),
      .height = body.height + (vertical ? radius * 2 : 0.F),
      .cornerRadius = radius,
      .bulgeRadius = radius,
      .outputX = static_cast<float>(body.x) - (vertical ? 0.F : radius),
      .outputY = static_cast<float>(body.y) - (vertical ? radius : 0.F),
      .outputWidth = body.width + (vertical ? 0.F : radius * 2),
      .outputHeight = body.height + (vertical ? radius * 2 : 0.F),
      .finalOutputX = static_cast<float>(body.x) - (vertical ? 0.F : radius),
      .finalOutputY = static_cast<float>(body.y) - (vertical ? radius : 0.F),
      .finalOutputWidth = body.width + (vertical ? 0.F : radius * 2),
      .finalOutputHeight = body.height + (vertical ? radius * 2 : 0.F),
      .revealProgress = m_attachedRevealProgress,
      .source = m_attachedSource,
  };
  if (placement.attached && m_islandMorph) {
    auto& geometry=*m_attachedPanelGeometry;
    geometry.x=static_cast<float>(body.x-placement.barOriginX);
    geometry.y=static_cast<float>(body.y-placement.barOriginY);
    geometry.outputX=geometry.finalOutputX=static_cast<float>(body.x);
    geometry.outputY=geometry.finalOutputY=static_cast<float>(body.y);
    geometry.width=geometry.outputWidth=geometry.finalOutputWidth=static_cast<float>(body.width);
    geometry.height=geometry.outputHeight=geometry.finalOutputHeight=static_cast<float>(body.height);
    geometry.bulgeRadius=0.F;
  }
  if (m_attachedContactShadow && !m_panelContactShadowNode && m_attachedRevealContentNode && m_activePanel->hasDecoration()) {
    auto shadow = ui::box({});
    auto& children = m_attachedRevealContentNode->children();
    const auto background = std::ranges::find_if(children, [this](const auto& child) { return child.get() == m_bgNode; });
    const auto index = background == children.end() ? 0U : static_cast<std::size_t>(std::distance(children.begin(), background) + 1);
    m_panelContactShadowNode = static_cast<Box*>(m_attachedRevealContentNode->insertChildAt(index, std::move(shadow)));
  }
  m_attachedPlacementPending = false;
  m_sceneGeometryDirty = true;
  if (placement.attached) applyAttachedDecorationStyle();
  else if (m_bgNode) static_cast<Box*>(m_bgNode)->setFill(scaleAlpha(placement.background,placement.opacity));
}

void PanelManager::onConfigReloaded() {
  m_persistentHost.onConfigReloaded();
  if (!isOpen() || m_config == nullptr || m_activePanel == nullptr) {
    return;
  }
  if (!m_attachedToBar && m_layerSurface != nullptr) {
    const LayerShellLayer panelLayer = resolveFloatingPanelLayer(m_config, m_activePanelId, *m_activePanel);
    if (panelLayer != m_panelLayer) {
      m_clickShield.setLayer(panelLayer);
      m_layerSurface->setLayer(panelLayer);
      m_panelLayer = panelLayer;
    }
  }

  m_activePanel->setContentScale(shell::panel_surface::contentScale(m_config));
  if (m_attachedToBar) {
    if (attachedAnimationDuration()<=0.0F) {
      if (m_attachedRevealClipNode) m_animations.cancelForOwner(m_attachedRevealClipNode);
      m_attachedOpenAnimationPending=false;
      m_attachedRevealProgress=m_closing?0.F:1.F;
      if (m_closing) {
        const auto generation=++m_destroyGeneration;
        DeferredCall::callLater([this,generation]{if(m_destroyGeneration==generation)destroyPanel();});
      }
    }
    // Defer shape/blur/input changes to the retained frame layout. A placement
    // request commits surface state before its configure, so do not pair a new
    // blur silhouette with the previously painted buffer here.
  } else {
    applyDetachedReveal(m_detachedRevealProgress);
  }
  const float panelBackgroundOpacity =
      m_attachedToBar ? m_attachedBackgroundOpacity : shell::panel_surface::backgroundOpacity(m_config);
  m_activePanel->setPanelCardOpacity(shell::panel_surface::cardOpacity(m_config, panelBackgroundOpacity));
  if (!m_attachedToBar && m_bgNode != nullptr) {
    auto* bg = static_cast<Box*>(m_bgNode);
    bg->setPanelStyle(m_config->config().shell.panel.borders);
    bg->setFill(m_attachedToBar ? scaleAlpha(m_attachedBackground, panelBackgroundOpacity) : colorSpecFromRole(ColorRole::Surface, panelBackgroundOpacity));
    if (m_config->config().shell.panel.borders) {
      bg->setBorder(colorSpecFromRole(ColorRole::Outline, panelBackgroundOpacity), Style::borderWidth);
    }
  }
  if (m_panelShadowNode != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    m_panelShadowNode->setVisible(panelShadow);
    if (!m_attachedToBar && panelShadow) {
      const float shadowRadius = Style::scaledRadiusXl(m_activePanel->contentScale());
      m_panelShadowNode->setStyle(
          shell::surface_shadow::style(
              shadowConfig, panelBackgroundOpacity,
              shell::surface_shadow::Shape{.radius = Radii{shadowRadius, shadowRadius, shadowRadius, shadowRadius}}
          )
      );
    }
  }
  if (m_surface != nullptr) {
    m_surface->requestUpdate();
  }

  // Content padding and insets can change without changing the wl_surface size.
  m_sceneGeometryDirty = true;
  refreshPanelPlacement();
}


void PanelManager::buildScene(std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("PanelManager::buildScene");
  if (m_renderContext == nullptr || m_activePanel == nullptr) {
    return;
  }
  layoutScene(m_surface->renderTarget().renderer(), width, height);
}

void PanelManager::layoutScene(Renderer& renderer, std::uint32_t width, std::uint32_t height) {
  m_sceneGeometryDirty = false;
  const bool hasDecoration = m_activePanel->hasDecoration();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);

  if (m_sceneRoot == nullptr) {
    m_sceneRoot = ui::node({});
    m_sceneRoot->setMaterialSurface("panel");
    m_sceneRoot->setAnimationManager(&m_animations);
    if (m_layerSurface != nullptr && m_renderContext != nullptr) {
      m_selectPopup = std::make_unique<SelectDropdownPopup>(m_platform->wayland(), *m_renderContext);
      if (m_config != nullptr) {
        m_selectPopup->setShadowConfig(m_config->config().shell.shadow);
      }
      m_selectPopup->setParent(m_layerSurface->layerSurface(), m_wlSurface, m_output);
      m_sceneRoot->setPopupContext(m_selectPopup.get());
    }
    m_sceneRoot->setSize(w, h);

    Node* sceneParent = m_sceneRoot.get();
    if (m_attachedToBar) {
      auto revealClip = ui::node({});
      revealClip->setClipChildren(true);
      m_attachedRevealClipNode = m_sceneRoot->addChild(std::move(revealClip));

      auto revealContent = ui::node({});
      m_attachedRevealContentNode = m_attachedRevealClipNode->addChild(std::move(revealContent));
      sceneParent = m_attachedRevealContentNode;
    } else {
      auto revealClip = ui::node({});
      revealClip->setClipChildren(true);
      m_detachedRevealClipNode = m_sceneRoot->addChild(std::move(revealClip));

      auto revealContent = ui::node({});
      m_detachedRevealContentNode = m_detachedRevealClipNode->addChild(std::move(revealContent));
      sceneParent = m_detachedRevealContentNode;
    }

    if (hasDecoration && m_config != nullptr) {
      auto shadow = ui::box({});
      m_panelShadowNode = static_cast<Box*>(sceneParent->addChild(std::move(shadow)));
      m_panelShadowNode->setZIndex(-1);
      m_panelShadowNode->setVisible(m_config->config().shell.panel.shadow);
    }

    if (hasDecoration) {
      auto bg = ui::box({});
      const bool panelBorders = m_config != nullptr && m_config->config().shell.panel.borders;
      bg->setMaterialIdentity("surface", "panel");
      bg->setPanelStyle(panelBorders);
      if (m_attachedToBar) {
        const float radius = Style::scaledRadiusXl(m_activePanel->contentScale());
        bg->clearBorder();
        bg->setCornerShapes(attached_panel::cornerShapes(m_attachedBarPosition));
        bg->setLogicalInset(attached_panel::logicalInset(m_attachedBarPosition, radius));
        bg->setRadii(Radii{radius, radius, radius, radius});
        // Fill (opacity-dependent) is applied via applyAttachedDecorationStyle() below.
      } else {
        const float backgroundOpacity = shell::panel_surface::backgroundOpacity(m_config);
        bg->setFill(colorSpecFromRole(ColorRole::Surface, backgroundOpacity));
        if (panelBorders) {
          bg->setBorder(colorSpecFromRole(ColorRole::Outline, backgroundOpacity), Style::borderWidth);
        }
      }
      m_bgNode = sceneParent->addChild(std::move(bg));
    }

    if (hasDecoration && m_attachedToBar && m_attachedContactShadow) {
      auto contactShadow = ui::box({});
      m_panelContactShadowNode = static_cast<Box*>(sceneParent->addChild(std::move(contactShadow)));
    }

    // Create panel content inside a wrapper node for staggered fade-in
    auto contentWrapper = ui::node({});
    m_contentNode = contentWrapper.get();
    m_activePanel->setAnimationManager(&m_animations);
    const float panelBackgroundOpacity =
        m_attachedToBar ? m_attachedBackgroundOpacity : shell::panel_surface::backgroundOpacity(m_config);
    m_activePanel->setPanelCardOpacity(shell::panel_surface::cardOpacity(m_config, panelBackgroundOpacity));
    m_activePanel->create();
    m_activePanel->onOpen(m_pendingOpenContext);
    m_pendingOpenContext.clear();
    if (m_activePanel->root() != nullptr) {
      contentWrapper->addChild(m_activePanel->releaseRoot());
    }
    if (m_attachedToBar) {
      auto contentClip=ui::node({});contentClip->setClipChildren(true);
      m_attachedContentClipNode=sceneParent->addChild(std::move(contentClip));
      m_attachedContentClipNode->addChild(std::move(contentWrapper));
    } else sceneParent->addChild(std::move(contentWrapper));

    m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
    m_inputDispatcher.setTextInputContext(m_wlSurface, m_platform->wayland().textInputService());
    m_inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    m_inputDispatcher.setHoverChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      if (m_layerSurface != nullptr) {
        TooltipManager::instance().onHoverChange(next, m_layerSurface->layerSurface(), m_output);
      }
    });
    m_inputDispatcher.setFocusChangeCallback([this](InputArea* /*old*/, InputArea* next) {
      if (m_activePanel != nullptr && next != nullptr) {
        m_activePanel->scrollFocusedInputIntoView(next);
      }
    });

    if (m_attachedToBar && m_attachedRevealClipNode != nullptr) {
      m_sceneRoot->setOpacity(1.0F);
      applyAttachedReveal(0.0F);
      m_attachedOpenAnimationPending = true;
    } else {
      applyDetachedReveal(0.0F);
      m_animations.animate(
          0.0F, 1.0F, Style::animNormal, Easing::EaseOutCubic, [this](float v) { applyDetachedReveal(v); }, {},
          m_sceneRoot.get()
      );
    }

    m_surface->setSceneRoot(m_sceneRoot.get());

    // Set initial keyboard focus if the panel requests it
    if (m_activePanel != nullptr) {
      if (auto* focusArea = m_activePanel->initialFocusArea(); focusArea != nullptr) {
        m_inputDispatcher.setFocus(focusArea);
      }
    }
  }

  m_sceneRoot->setSize(w, h);
  if (!m_attachedToBar && m_attachedContentClipNode) {
    m_attachedContentClipNode->setPosition(0,0);
    m_attachedContentClipNode->setFrameSize(w,h);
  }
  if (m_attachedRevealContentNode != nullptr) {
    m_attachedRevealContentNode->setFrameSize(w, h);
  }
  if (m_detachedRevealContentNode != nullptr) {
    m_detachedRevealContentNode->setFrameSize(w, h);
  }

  // Honor the compositor-configured surface size: a filled axis derives its
  // visual size from the configure (surface minus shadow bleed), and a fixed
  // axis the compositor configured smaller than requested lays out at the
  // configured size instead of overflowing the buffer.
  if (!m_attachedToBar) {
    const std::int32_t availW = static_cast<std::int32_t>(width) - m_panelInsetX - m_detachedBleedRight;
    const std::int32_t availH = static_cast<std::int32_t>(height) - m_panelInsetY - m_detachedBleedBottom;
    if (availW > 0 && (m_panelFillWidth || m_panelVisualWidth > static_cast<std::uint32_t>(availW))) {
      m_panelVisualWidth = static_cast<std::uint32_t>(availW);
    }
    if (availH > 0 && (m_panelFillHeight || m_panelVisualHeight > static_cast<std::uint32_t>(availH))) {
      m_panelVisualHeight = static_cast<std::uint32_t>(availH);
    }
    if (m_surface != nullptr) {
      m_surface->setInputRegion({InputRect{
          m_panelInsetX, m_panelInsetY, static_cast<int>(m_panelVisualWidth), static_cast<int>(m_panelVisualHeight)
      }});
    }
  }

  if (m_attachedToBar) {
    applyAttachedReveal(m_attachedRevealProgress);
  } else {
    applyDetachedReveal(m_detachedRevealProgress);
  }

  const auto panelX = static_cast<float>(m_panelInsetX);
  const auto panelY = static_cast<float>(m_panelInsetY);
  const float panelW = m_panelVisualWidth > 0 ? static_cast<float>(m_panelVisualWidth) : w;
  const float panelH = m_panelVisualHeight > 0 ? static_cast<float>(m_panelVisualHeight) : h;
  const float attachedRadius = m_attachedToBar ? Style::scaledRadiusXl(m_activePanel->contentScale()) : 0.0F;
  const bool barIsVertical = m_attachedToBar && (m_attachedBarPosition == "left" || m_attachedBarPosition == "right");
  // The bg extends past the body along the bar cross axis for concave-corner notches.
  const float bgX = barIsVertical ? panelX : panelX - attachedRadius;
  const float bgY = barIsVertical ? panelY - attachedRadius : panelY;
  const float bgW = barIsVertical ? panelW : panelW + attachedRadius * 2.0F;
  const float bgH = barIsVertical ? panelH + attachedRadius * 2.0F : panelH;

  if (m_panelShadowNode != nullptr && m_config != nullptr) {
    const auto& shadowConfig = m_config->config().shell.shadow;
    const bool panelShadow =
        m_config->config().shell.panel.shadow && shell::surface_shadow::enabled(true, shadowConfig);
    m_panelShadowNode->setVisible(panelShadow);
    const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
    const auto shadowOffsetX = static_cast<float>(shadowOff.x);
    const auto shadowOffsetY = static_cast<float>(shadowOff.y);
    m_panelShadowNode->setPosition(bgX + shadowOffsetX, bgY + shadowOffsetY);
    m_panelShadowNode->setSize(bgW, bgH);
    if (!m_attachedToBar && panelShadow) {
      const float shadowRadius = Style::scaledRadiusXl(m_activePanel->contentScale());
      const float panelBackgroundOpacity = shell::panel_surface::backgroundOpacity(m_config);
      m_panelShadowNode->setStyle(
          shell::surface_shadow::style(
              shadowConfig, panelBackgroundOpacity,
              shell::surface_shadow::Shape{.radius = Radii{shadowRadius, shadowRadius, shadowRadius, shadowRadius}}
          )
      );
    }
  }

  if (m_bgNode != nullptr) {
    m_bgNode->setPosition(bgX, bgY);
    m_bgNode->setSize(bgW, bgH);
  }

  if (m_panelContactShadowNode != nullptr) {
    constexpr float kContactShadowBaseThickness = 16.0F;
    const float scale = m_activePanel->contentScale();
    const float contactThickness =
        std::min(std::max(kContactShadowBaseThickness * scale, attachedRadius * 2.0F), barIsVertical ? bgW : bgH);
    const bool barIsBottom = m_attachedBarPosition == "bottom";
    const bool barIsRight = m_attachedBarPosition == "right";
    float contactX = bgX;
    float contactY = bgY;
    float contactW = bgW;
    float contactH = bgH;
    if (barIsVertical) {
      contactW = contactThickness;
      if (barIsRight) {
        contactX = bgX + bgW - contactThickness;
      }
    } else {
      contactH = contactThickness;
      if (barIsBottom) {
        contactY = bgY + bgH - contactThickness;
      }
    }
    m_panelContactShadowNode->setPosition(contactX, contactY);
    m_panelContactShadowNode->setSize(contactW, contactH);
  }

  // Re-apply opacity-dependent styling for bg, shadow, and contact-shadow.
  // Ensures these stay in sync if the bar config changed.
  if (m_attachedToBar) {
    applyAttachedDecorationStyle();
  }

  const float kPadding = hasDecoration ? m_activePanel->contentScale() * Style::panelPadding : 0.0F;
  m_contentWidth = std::max(1.0F, panelW - kPadding * 2.0F);
  m_contentHeight = std::max(1.0F, panelH - kPadding * 2.0F);
  {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_activePanel->update(renderer);
  }
  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    m_activePanel->layout(renderer, m_contentWidth, m_contentHeight);
  }
  if (m_contentNode != nullptr) {
    m_contentNode->setPosition(panelX + kPadding, panelY + kPadding);
    m_contentNode->setSize(m_contentWidth, m_contentHeight);
  }
  if (m_attachedToBar) applyAttachedReveal(m_attachedRevealProgress);
  applyPendingPanelFocus();
  if (m_pointerInside) {
    m_inputDispatcher.syncPointerHover();
  }
}

void PanelManager::applyPendingPanelFocus() {
  if (m_activePanel == nullptr) {
    return;
  }
  if (auto* area = m_activePanel->takePendingFocusArea(); area != nullptr) {
    m_inputDispatcher.setFocus(area);
  }
}

void PanelManager::prepareFrame(bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || m_surface == nullptr) {
    return;
  }
  if (m_activePanel == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(m_surface->renderTarget());
  Renderer& renderer = m_surface->renderTarget().renderer();

  const auto width = m_surface->width();
  const auto height = m_surface->height();

  if (m_attachedPlacementPending && !m_attachedAwaitingConfigure) applyPanelPlacement(width, height);
  const bool needsSceneBuild = m_sceneGeometryDirty || m_sceneRoot == nullptr
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->width())) != width
      || static_cast<std::uint32_t>(std::round(m_sceneRoot->height())) != height;
  if (needsSceneBuild) {
    buildScene(width, height);
  }

  if (!needsSceneBuild && needsUpdate) {
    UiPhaseScope updatePhase(UiPhase::Update);
    m_activePanel->update(renderer);
  }
  if (!needsSceneBuild && needsLayout) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    if (m_activePanel != nullptr) {
      m_activePanel->layout(renderer, m_contentWidth, m_contentHeight);
    }
    if (m_pointerInside) {
      m_inputDispatcher.syncPointerHover();
    }
  }
  if (!needsSceneBuild && (needsUpdate || needsLayout)) {
    applyPendingPanelFocus();
  }
}

void PanelManager::registerIpc(IpcService& ipc) {
  auto parseOpenArgs = [](std::string_view rawArgs, std::string_view command, std::string& panelId,
                          std::string& context) -> std::optional<std::string> {
    const std::string_view args = StringUtils::trimLeftView(rawArgs);
    if (args.empty()) {
      return "error: " + std::string(command) + " requires a panel id\n";
    }

    const auto sep = args.find_first_of(" \t\n\r\f\v");
    if (sep == std::string_view::npos) {
      panelId = std::string(args);
      context.clear();
      return std::nullopt;
    }

    panelId = std::string(args.substr(0, sep));
    // Preserve the context verbatim (only strip the separator's leading
    // whitespace) — trailing whitespace can be significant, e.g. a command.
    context = std::string(StringUtils::trimLeftView(args.substr(sep + 1)));
    return std::nullopt;
  };

  auto unknownPanelError = [this](std::string_view panelId) -> std::string {
    std::vector<std::string> ids;
    ids.reserve(m_panels.size());
    for (const auto& entry : m_panels) {
      ids.push_back(entry.first);
    }
    m_persistentHost.appendPanelIds(ids);
    std::ranges::sort(ids);

    std::string error = "error: unknown panel \"" + std::string(panelId) + "\"";
    if (!ids.empty()) {
      error += " (available: " + StringUtils::join(ids, ", ") + ")";
    }
    error += '\n';
    return error;
  };

  ipc.bind(
      noctalia::cli::msg::panelToggle,
      [this, parseOpenArgs, unknownPanelError](const std::string& args) -> std::string {
        std::string panelId;
        std::string context;
        if (auto error = parseOpenArgs(args, "panel-toggle", panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId) && !m_persistentHost.hasPanel(panelId)) {
          return unknownPanelError(panelId);
        }
        // Output left unset: openPanel resolves it (focus source, else compositor probe).
        if (context.empty()) {
          togglePanel(panelId);
        } else {
          togglePanel(panelId, PanelOpenRequest{.context = context});
        }
        return "ok\n";
      }
  );

  ipc.bind(
      noctalia::cli::msg::panelOpen, [this, parseOpenArgs, unknownPanelError](const std::string& args) -> std::string {
        std::string panelId;
        std::string context;
        if (auto error = parseOpenArgs(args, "panel-open", panelId, context)) {
          return *error;
        }
        if (!m_panels.contains(panelId) && !m_persistentHost.hasPanel(panelId)) {
          return unknownPanelError(panelId);
        }

        if (isOpen() && !m_closing && m_activePanelId == panelId) {
          if (!context.empty() && m_activePanel != nullptr) {
            m_activePanel->onOpen(context);
            refresh();
          }
          return "ok\n";
        }

        // Output left unset: openPanel resolves it (focus source, else compositor probe).
        openPanel(panelId, PanelOpenRequest{.context = context});
        return "ok\n";
      }
  );

  ipc.bind(noctalia::cli::msg::panelClose, [this, unknownPanelError](const std::string& args) -> std::string {
    const std::string panelId = StringUtils::trim(args);
    if (!panelId.empty() && StringUtils::splitWhitespace(panelId).size() != 1) {
      return "error: panel-close accepts at most one panel id\n";
    }
    if (!panelId.empty() && !m_panels.contains(panelId) && !m_persistentHost.hasPanel(panelId)) {
      return unknownPanelError(panelId);
    }

    if (!panelId.empty() && m_persistentHost.hasPanel(panelId)) {
      m_persistentHost.close(panelId);
      return "ok\n";
    }
    if (panelId.empty() || isOpenPanel(panelId)) {
      closePanel();
    }
    return "ok\n";
  });

  const auto rejectSettingsArgs = [](const std::string& args, std::string_view command) -> std::optional<std::string> {
    if (StringUtils::trim(args).empty()) {
      return std::nullopt;
    }
    return std::format("error: {} accepts no arguments\n", command);
  };

  ipc.bind(noctalia::cli::msg::settingsOpen, [this](const std::string& args) -> std::string {
    openSettingsWindow(std::string(StringUtils::trimLeftView(args)));
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::settingsOpenWidget, [this, &ipc](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    std::string barName;
    std::string widgetName;
    if (parts.size() == 2) {
      barName = parts[0];
      widgetName = parts[1];
    } else if (parts.empty()) {
      // Invoked from a bar widget gesture: the widget is the implicit target.
      const auto& context = ipc.invocationContext();
      if (!context.has_value() || context->widgetName.empty()) {
        return "error: settings-open-widget needs <bar-name> <widget-name> unless invoked from a bar widget\n";
      }
      barName = context->barName;
      widgetName = context->widgetName;
    } else {
      return "error: settings-open-widget takes either no arguments or <bar-name> <widget-name>\n";
    }

    if (!m_openWidgetSettings) {
      return "error: settings window unavailable\n";
    }
    if (isOpen()) {
      closePanel();
    }
    m_openWidgetSettings(std::move(barName), std::move(widgetName));
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::settingsOpenPlugin, [this](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() != 1) {
      return "error: settings-open-plugin takes <plugin-id> (e.g. noctalia/notes)\n";
    }
    const std::string& pluginId = parts[0];
    if (!scripting::isValidPluginId(pluginId)) {
      return std::format("error: \"{}\" is not a plugin id (expected author/plugin)\n", pluginId);
    }

    if (!m_openPluginSettings) {
      return "error: settings window unavailable\n";
    }
    if (!openPluginSettings(pluginId)) {
      return std::format("error: plugin \"{}\" is not enabled or has no settings\n", pluginId);
    }
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::settingsClose, [this, rejectSettingsArgs](const std::string& args) -> std::string {
    if (auto error = rejectSettingsArgs(args, "settings-close")) {
      return *error;
    }
    closeSettingsWindow();
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::settingsToggle, [this](const std::string& args) -> std::string {
    toggleSettingsWindow(std::string(StringUtils::trimLeftView(args)));
    return "ok\n";
  });
}
