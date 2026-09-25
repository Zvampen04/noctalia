#pragma once
#include "ui/palette.h"

#include "core/timer_manager.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "shell/panel/attached_panel_context.h"
#include "shell/panel/attached_panel_morph.h"
#include "shell/panel/attached_panel_layout.h"
#include "shell/panel/panel_click_shield.h"
#include "shell/panel/persistent_panel_host.h"
#include "ui/dialogs/layer_popup_host.h"
#include "wayland/hyprland/popup_grab_host.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

class ConfigService;
class CompositorPlatform;
class ContextMenuPopup;
class SelectDropdownPopup;
class Box;
class IpcService;
class FocusGrab;
class LayerSurface;
class Node;
class RenderProxyNode;
class CountdownRingNode;
class Panel;
class RenderContext;
class Renderer;
class Surface;
class WaylandConnection;
enum class LayerShellLayer : std::uint32_t;
struct KeyboardEvent;
struct PointerEvent;
struct wl_output;
struct wl_surface;

struct PanelOpenRequest {
  wl_output* output = nullptr;
  float anchorX = 0.0F;
  float anchorY = 0.0F;
  bool hasExplicitAnchor = false;
  bool hasAnchorPosition = false;
  std::string_view context;
  std::string_view sourceBarName;
  AttachedPanelSource source;
};

class PanelManager : public PopupGrabHost {
public:
  PanelManager();
  ~PanelManager();

  PanelManager(const PanelManager&) = delete;
  PanelManager& operator=(const PanelManager&) = delete;

  static PanelManager& instance();
  static PanelManager* current() noexcept;

  void initialize(CompositorPlatform& platform, ConfigService* config, RenderContext* renderContext);

  // Optional: invoked from shell UI (e.g. control center) to spawn the standalone settings toplevel.
  void setOpenSettingsWindowCallback(std::function<void(std::string)> callback);
  void setOpenWidgetSettingsCallback(std::function<void(std::string barName, std::string widgetName)> callback);
  // Returns false when the plugin is unknown, disabled, or exposes no settings.
  void setOpenPluginSettingsCallback(std::function<bool(std::string pluginId)> callback);
  void setCloseSettingsWindowCallback(std::function<void()> callback);
  void setToggleSettingsWindowCallback(std::function<void(std::string)> callback);
  void setCloseDesktopWidgetsEditorCallback(std::function<void()> callback);
  void openSettingsWindow(std::string context = "");
  // Closes any open panel, then opens the settings window at the plugin's settings.
  // False when the settings window is unavailable, or the plugin is unknown, disabled, or
  // exposes no settings.
  [[nodiscard]] bool openPluginSettings(const std::string& pluginId);
  void closeSettingsWindow();
  void toggleSettingsWindow(std::string context = "");
  void setAttachedPanelGeometryCallback(
      std::function<void(wl_output*, std::string_view, std::optional<AttachedPanelGeometry>)> callback
  );
  // Callback to query the bar surface rects on a given output, in output-local
  // coordinates. The click shield's input region excludes these rects so
  // clicks on bar widgets keep flowing to the bar while a panel is open.
  void setClickShieldExcludeRectsProvider(std::function<std::vector<InputRect>(wl_output*)> provider);
  // Callback returning every bar wl_surface. Used to seed the Hyprland focus
  // grab whitelist so bar widgets keep receiving clicks while a panel is open.
  void setFocusGrabBarSurfacesProvider(std::function<std::vector<wl_surface*>()> provider);
  void setPanelClosedCallback(std::function<void()> callback);
  void setPanelOpenedCallback(std::function<void()> callback);
  void setAttachedPanelAvailabilityCallback(std::function<bool(wl_output*, std::string_view)> callback);
  void setAttachedPanelLayerProvider(std::function<std::optional<std::string>(wl_output*, std::string_view)> provider);
  void setAttachedPanelBarSettledCallback(std::function<bool(wl_output*, std::string_view)> callback);
  void setAttachedSourceGeometryProvider(
      std::function<std::optional<AttachedPanelSource>(wl_output*, std::string_view, const AttachedPanelSource&)> provider) {
    m_attachedSourceGeometryProvider = std::move(provider);
  }
  void setAttachedSourceContentProvider(std::function<const Node*(wl_output*,std::string_view,const AttachedPanelSource&)> provider) {
    m_attachedSourceContentProvider=std::move(provider);
  }
  // Called when an auto-hide bar finishes revealing for an attached panel open.
  void onAttachedBarRevealSettled(wl_output* output, std::string_view barName);

  void registerPanel(const std::string& id, std::unique_ptr<Panel> content);
  // Drops a previously registered panel, closing it first if it is open. Used to
  // retire plugin-backed panels on a plugin enable/disable/reload.
  void unregisterPanel(const std::string& id);

  void openPanel(const std::string& panelId, PanelOpenRequest request = {});
  // Change content without destroying its surface, source island or reveal state.
  void navigatePanelContext(const std::string& panelId, std::string context);
  void closePanel(bool animateClose = true, std::function<void()> afterClosed = {});
  void togglePanel(const std::string& panelId, PanelOpenRequest request);
  // IPC-friendly overload: asks CompositorPlatform for preferred interactive output.
  void togglePanel(const std::string& panelId);

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);

  [[nodiscard]] bool isOpen() const noexcept;
  [[nodiscard]] bool isOpenPanel(std::string_view panelId) const noexcept;
  [[nodiscard]] bool isPanelTransitionActive() const noexcept;
  [[nodiscard]] bool isAttachedOpen() const noexcept;
  // Output the active panel is on; null when none is open.
  [[nodiscard]] wl_output* attachedPanelOutput() const noexcept;
  // Bar that opened the active panel; empty when none was recorded.
  [[nodiscard]] std::string_view attachedSourceBarName() const noexcept;
  [[nodiscard]] const std::string& activePanelId() const noexcept;
  [[nodiscard]] std::string_view activePanelContext() const noexcept;
  [[nodiscard]] Panel* activePanel() const noexcept { return m_activePanel; }
  // True when a panel is open and it reports the given context as active (e.g. control-center tab).
  [[nodiscard]] bool isActivePanelContext(std::string_view context) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> popupParentContextForSurface(wl_surface* surface) const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext> fallbackPopupParentContext() const noexcept;
  [[nodiscard]] std::optional<LayerPopupParentContext>
  popupParentContextForPanel(std::string_view panelId) const noexcept;

  [[nodiscard]] RenderContext* renderContext() const noexcept { return m_renderContext; }
  [[nodiscard]] WaylandConnection* wayland() const noexcept;

  // Applies the shell's effective popup shadow settings to a panel-owned
  // context menu before it opens.
  void configureContextMenuPopup(ContextMenuPopup& popup) const;
  void setActivePopup(ContextMenuPopup* popup);
  void clearActivePopup();

  void refresh();
  // Re-read preferredWidth/Height without replacing the active content tree.
  // Attached panels retain their bar anchor, input and evolving material geometry.
  void relayoutActivePanelPreferredSize();
  // Refresh a single panel by id, whichever host owns it. Used by content that
  // knows which panel it belongs to (e.g. a plugin panel's new UI tree).
  void refreshPanel(std::string_view panelId);
  // Close a panel by id, whichever host owns it.
  void closePanelById(std::string_view panelId);
  // Arms the next frame tick for a panel by id, whichever host owns it. Requests
  // a redraw: that queues a frame and flags the frame callback to run the panel's
  // onFrameTick, so a panel can sustain its own animation loop without knowing
  // which host it lives in.
  void requestAnimationFrameForPanel(std::string_view panelId);
  // Reacts to a ConfigService reload while a panel is open: re-pulls the host bar's
  // per-panel-relevant config (attached background opacity), styling, and compositor
  // blur region. No-op when no panel is open.
  void onConfigReloaded();
  void onIconThemeChanged();
  void focusArea(InputArea* area);
  [[nodiscard]] InputDispatcher& inputDispatcher() noexcept { return m_inputDispatcher; }
  [[nodiscard]] const InputDispatcher& inputDispatcher() const noexcept { return m_inputDispatcher; }
  void requestUpdateOnly();
  void requestLayout();
  // Requests a redraw on the active panel surface without re-running panel
  // update/layout. Used for reactive palette restyling.
  void requestRedraw();
  void requestFrameTick();
  void close();
  void beginAttachedPopup(wl_surface* surface);
  void endAttachedPopup(wl_surface* surface);

  // PopupGrabHost. PopupSurface enrolls itself with us while our focus_grab
  // is active so the compositor doesn't fire `cleared` when the user
  // interacts with a popup opened from inside the panel.
  void registerPopupSurface(wl_surface* surface) override;
  void unregisterPopupSurface(wl_surface* surface) override;

  void registerIpc(IpcService& ipc);

private:
  void applyPreferredPanelSize();
  [[nodiscard]] float preferredPanelWidth() const;
  [[nodiscard]] float preferredPanelHeight() const;
  std::optional<std::pair<float,float>> m_resizeSize;
  std::optional<std::pair<float,float>> m_resizeTarget;
  std::uint32_t m_resizeAnimationId = 0;
  friend class PanelManagerLayoutTestAccess;
  struct PlacementRequest {
    std::uint32_t anchor = 0;
    int top = 0, right = 0, bottom = 0, left = 0, exclusiveZone = 0;
    bool operator==(const PlacementRequest&) const = default;
  };
  struct RetainedPlacement {
    bool attached = true;
    bool screenEdge = false;
    bool islandMorph = false;
    bool anchoredRight = false, anchoredBottom = false;
    bool fillWidth = false, fillHeight = false;
    std::string barName;
    std::string position;
    attached_panel::BodyRect body{};
    int insetX = 0, insetY = 0, trailingX = 0, trailingY = 0;
    std::uint32_t surfaceWidth = 1, surfaceHeight = 1;
    int barOriginX = 0, barOriginY = 0;
    LayerShellLayer layer = LayerShellLayer::Top;
    ColorSpec background = colorSpecFromRole(ColorRole::Surface);
    float opacity = 1.0F;
    bool contactShadow = false;
    AttachedRevealDirection detachedDirection = AttachedRevealDirection::Down;
    std::optional<PlacementRequest> request;
  };
  void refreshPanelPlacement();
  void queuePanelPlacement(RetainedPlacement placement, std::uint32_t anchor,
                           int top, int right, int bottom, int left, int exclusiveZone);
  void applyAttachmentMode(bool attached);
  void applyPanelPlacement(std::uint32_t width, std::uint32_t height);
  void onSurfaceConfigured();
  static PanelManager* s_instance;

  void buildScene(std::uint32_t width, std::uint32_t height);
  void layoutScene(Renderer& renderer, std::uint32_t width, std::uint32_t height);
  void prepareFrame(bool needsUpdate, bool needsLayout);
  void applyPendingPanelFocus();
  void destroyPanel();
  // Called before the panel surface commits so outside-click dismissal is ready
  // for its first frame. The panel rect is excluded separately because
  // same-layer stacking order is compositor-defined.
  void activateClickShield(LayerShellLayer layer);
  // Called AFTER the panel surface is mapped so the panel wl_surface is
  // available for the whitelist. No-op when focus-grab is unavailable.
  void activateFocusGrab();
  void deactivateOutsideClickHandlers();
  void applyAttachedReveal(float progress);
  void applyAttachedMorph(float progress);
  [[nodiscard]] bool attachedMorphEnabled() const;
  [[nodiscard]] float attachedAnimationDuration() const;
  [[nodiscard]] attached_panel::MorphGeometry attachedMorphGeometry(float progress) const;
  void applyDetachedReveal(float progress);
  void startAttachedOpenAnimation();
  void publishAttachedPanelGeometry(float revealProgress);
  // Restyle the attached-panel decoration nodes (bg fill, drop shadow, contact shadow)
  // using the cached attached background opacity and bar position. Geometry/positions are not touched.
  // Safe to call any time after buildScene has run.
  void applyAttachedDecorationStyle();
  // Submit a wl_region matching the panel body after applying the current reveal clip.
  void applyPanelCompositorBlur(int bodyX, int bodyY, int bodyW, int bodyH, int clipX, int clipY, int clipW, int clipH, float shapeRadius = -1.0F);

  CompositorPlatform* m_platform = nullptr;
  ConfigService* m_config = nullptr;
  RenderContext* m_renderContext = nullptr;
  std::function<void(std::string)> m_openSettingsWindow;
  std::function<void(std::string, std::string)> m_openWidgetSettings;
  std::function<bool(std::string)> m_openPluginSettings;
  std::function<void()> m_closeSettingsWindow;
  std::function<void(std::string)> m_toggleSettingsWindow;
  std::function<void()> m_closeDesktopWidgetsEditor;
  std::function<void(wl_output*, std::string_view, std::optional<AttachedPanelGeometry>)>
      m_attachedPanelGeometryCallback;
  std::function<std::vector<InputRect>(wl_output*)> m_clickShieldExcludeRectsProvider;
  std::function<std::vector<wl_surface*>()> m_focusGrabBarSurfacesProvider;
  std::function<void()> m_panelClosedCallback;
  std::function<void()> m_afterCloseCallback;
  std::function<void()> m_panelOpenedCallback;
  std::function<bool(wl_output*, std::string_view)> m_attachedPanelAvailabilityCallback;
  std::function<std::optional<std::string>(wl_output*, std::string_view)> m_attachedPanelLayerProvider;
  std::function<bool(wl_output*, std::string_view)> m_attachedPanelBarSettledCallback;
  PanelClickShield m_clickShield;
  PersistentPanelHost m_persistentHost;
  std::unique_ptr<FocusGrab> m_focusGrab;

  std::unique_ptr<Surface> m_surface;
  LayerSurface* m_layerSurface = nullptr;
  LayerShellLayer m_panelLayer = LayerShellLayer::Top;
  // m_sceneRoot must be destroyed before m_animations — ~Node() calls cancelForOwner().
  // Also m_panels (which own their own Nodes parented under m_sceneRoot) must be destroyed
  // before m_animations for the same reason.
  AnimationManager m_animations;
  std::unique_ptr<Node> m_sceneRoot;
  Node* m_bgNode = nullptr;
  Node* m_contentNode = nullptr;
  Node* m_detachedRevealClipNode = nullptr;
  Node* m_detachedRevealContentNode = nullptr;
  Node* m_attachedRevealClipNode = nullptr;
  Node* m_attachedRevealContentNode = nullptr;
  Node* m_attachedContentClipNode = nullptr;
  RenderProxyNode* m_islandOpenerProxy = nullptr;
  CountdownRingNode* m_islandUsageRing = nullptr;
  std::function<std::optional<AttachedPanelSource>(wl_output*, std::string_view, const AttachedPanelSource&)> m_attachedSourceGeometryProvider;
  std::function<const Node*(wl_output*,std::string_view,const AttachedPanelSource&)> m_attachedSourceContentProvider;
  Box* m_panelShadowNode = nullptr;
  Box* m_panelContactShadowNode = nullptr;
  InputDispatcher m_inputDispatcher;

  std::unordered_map<std::string, std::unique_ptr<Panel>> m_panels;
  Panel* m_activePanel = nullptr;
  std::string m_activePanelId;
  std::string m_pendingOpenContext;

  wl_output* m_output = nullptr;
  wl_surface* m_wlSurface = nullptr;
  float m_contentWidth = 0.0F;
  float m_contentHeight = 0.0F;
  std::int32_t m_panelInsetX = 0;
  std::int32_t m_panelInsetY = 0;
  std::uint32_t m_panelVisualWidth = 0;
  std::uint32_t m_panelVisualHeight = 0;
  std::optional<InputRect> m_panelOutputInputRect;
  // Fill axes derive their visual size from the compositor-configured surface
  // size in buildScene; that math also needs the trailing shadow bleed.
  bool m_panelFillWidth = false;
  bool m_panelFillHeight = false;
  std::int32_t m_detachedBleedRight = 0;
  std::int32_t m_detachedBleedBottom = 0;
  std::int32_t m_attachedBleedRight=0,m_attachedBleedBottom=0;
  std::int32_t m_attachedBarOriginX=0,m_attachedBarOriginY=0;
  ColorSpec m_attachedBackground = colorSpecFromRole(ColorRole::Surface);
  float m_attachedBackgroundOpacity = 1.0F;
  bool m_attachedContactShadow = false;
  float m_attachedRevealProgress = 1.0F;
  float m_detachedRevealProgress = 1.0F;
  AttachedRevealDirection m_attachedRevealDirection = AttachedRevealDirection::Down;
  AttachedRevealDirection m_detachedRevealDirection = AttachedRevealDirection::Down;
  Timer m_keyboardRelaxTimer;
  std::string m_attachedBarPosition; // "top" / "bottom" / "left" / "right" while attached, empty otherwise
  bool m_attachedHasAnchor = false;
  bool m_attachedAnchorAvailable = false;
  std::string m_openingSourceBarName;
  std::optional<RetainedPlacement> m_attachedPlacement;
  std::uint64_t m_attachedPlacementGeneration = 0;
  bool m_attachedPlacementPending = false;
  bool m_attachedAwaitingConfigure = false;
  bool m_refreshingRetainedPlacement = false;
  bool m_sceneGeometryDirty = false;
  float m_attachedAnchorX = 0.0F;
  float m_attachedAnchorY = 0.0F;
  std::string m_sourceBarName;       // resolved bar currently owning the attached join
  AttachedPanelSource m_attachedSource;
  bool m_islandMorph = false;
  std::optional<AttachedPanelGeometry> m_attachedPanelGeometry;
  bool m_pointerInside = false;
  bool m_inTransition = false;
  bool m_closing = false;
  bool m_attachedToBar = false;
  bool m_screenEdgeAttachment = false;
  bool m_attachedOpenAnimationPending = false;
  std::size_t m_attachedPopupCount = 0;
  ContextMenuPopup* m_activePopup = nullptr;
  std::unique_ptr<SelectDropdownPopup> m_selectPopup;
  std::uint64_t m_destroyGeneration = 0; // invalidates stale deferred destroyPanel calls
};
