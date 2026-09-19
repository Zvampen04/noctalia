#pragma once

#include "config/config_types.h"
#include "render/animation/animation_manager.h"
#include "render/scene/input_dispatcher.h"
#include "ui/controls/context_menu.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/popup_parent.h"
#include "ui/signal.h"
#include "ui/material_target_catalog.h"
#include "ui/popup_transition.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Node;
class Box;
class RectNode;
class PopupSurface;
class RenderContext;
class WaylandConnection;
struct KeyboardEvent;
struct PointerEvent;
struct wl_surface;
class ContextMenuPopupTestAccess;

struct ContextMenuPopupPlacement {
  std::uint32_t anchor = 0;
  std::uint32_t gravity = 0;
  std::int32_t offsetX = 0;
  std::int32_t offsetY = 0;
  popup_chrome::Attachment chromeAttachment{
      .horizontal = popup_chrome::HorizontalAttachment::Center,
      .vertical = popup_chrome::VerticalAttachment::Top,
  };
};

struct ContextMenuPopupRequest {
  std::vector<ContextMenuControlEntry> entries;
  // <= 0 sizes the menu to its widest entry, clamped to [minMenuWidth, maxMenuWidth] (0 = unbounded).
  float menuWidth = 0.0F;
  float minMenuWidth = 0.0F;
  float maxMenuWidth = 0.0F;
  // Scales row heights, fonts, and insets (select-style dropdowns derive this from their font size).
  float contentScale = 1.0F;
  std::size_t maxVisible = 0;
  // Entry highlighted (and scrolled into view) when the menu opens; out-of-range falls back to the
  // first interactive entry.
  std::size_t initialHighlight = static_cast<std::size_t>(-1);
  PopupAnchorRect anchor;
  PopupSurfaceParent parent;
  // Surface whose pointer events are translated into menu coordinates while a press holds the
  // pointer capture (scrollbar thumb drags leaving the popup). Defaults to parent.wlSurface.
  wl_surface* pointerParentSurface = nullptr;
  // Serial of the input event that requested this popup. When absent, legacy
  // callers retain the previous last-input-serial behavior.
  std::optional<std::uint32_t> inputSerial = std::nullopt;
  std::optional<ContextMenuPopupPlacement> placement = std::nullopt;
};

class ContextMenuPopup {
public:
  ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext);
  ~ContextMenuPopup();

  void open(ContextMenuPopupRequest request);
  void close();
  [[nodiscard]] bool isOpen() const noexcept;

  void setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback);
  void setOnDismissed(std::function<void()> callback);
  // Refresh an open menu without recreating its parent/grab. Stable entry IDs
  // retain keyboard selection; stale queued actions and presses are cancelled.
  void setEntries(std::vector<ContextMenuControlEntry> entries);
  void refreshStyle(float scale);
  void setShadowConfig(const ShellConfig::ShadowConfig& shadow);
  // Select dropdowns reuse this host but expose a distinct registered material
  // class. Only catalog-backed class targets are accepted.
  void setMaterialClassTarget(std::string_view target);

  bool onPointerEvent(const PointerEvent& event);
  void onKeyboardEvent(const KeyboardEvent& event);
  [[nodiscard]] wl_surface* wlSurface() const noexcept;
  [[nodiscard]] xdg_surface* xdgSurface() const noexcept;
  [[nodiscard]] std::uint32_t width() const noexcept;
  [[nodiscard]] std::uint32_t height() const noexcept;

  // Route a keyboard event to the currently-open context menu, if any. A grab
  // popup is modal, so while one is open it swallows keys (returns true).
  static bool dispatchKeyboardEvent(const KeyboardEvent& event);

private:
  friend class ContextMenuPopupTestAccess;

  WaylandConnection& m_wayland;
  RenderContext& m_renderContext;
  std::unique_ptr<PopupSurface> m_surface;
  AnimationManager m_animations;
  popup_transition::Transition m_transition;
  std::unique_ptr<Node> m_sceneRoot;
  Node* m_transitionRoot = nullptr;
  InputDispatcher m_inputDispatcher;
  ScrollViewState m_scrollState{};
  ScrollView* m_scrollView = nullptr;
  ContextMenuControl* m_menu = nullptr;
  std::size_t m_highlightedIndex = 0;
  std::shared_ptr<std::vector<ContextMenuControlEntry>> m_entries;
  std::uint64_t m_generation = 0;
  wl_surface* m_wlSurface = nullptr;
  wl_surface* m_pointerParentSurface = nullptr;
  bool m_pointerInside = false;

  void restoreParentKeyboardInteractivity();
  void ensureHighlightedVisible();
  void requestVisualUpdate();
  bool applyPendingStyle();
  void deferActivation(ContextMenuControlEntry entry);
  void deferClose();

  std::function<void(const ContextMenuControlEntry&)> m_onActivate;
  std::function<void()> m_onDismissed;
  ShellConfig::ShadowConfig m_shadowConfig;
  std::function<void(float)> m_refreshStyle;
  float m_contentScale = 1.0F;
  bool m_styleDirty = false;
  bool m_revealHighlight = false;
  Box* m_background = nullptr;
  RectNode* m_panelShadow = nullptr;
  Signal<>::ScopedConnection m_styleConnection;
  std::string m_materialClassTarget = "popup.context-menu";
  Style::MaterialTargetRegistration m_materialRegistration;
  // Deferred popup callbacks must not dereference this after an owner (for
  // example, a plugin panel being unregistered) destroys the popup.
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);

  // Parent layer surface (e.g. a bar) whose keyboard interactivity is flipped to
  // OnDemand while the menu is open so the grabbing popup inherits keyboard
  // focus, then restored to None on close.
  zwlr_layer_surface_v1* m_keyboardParentLayerSurface = nullptr;
  wl_surface* m_keyboardParentWlSurface = nullptr;

  static ContextMenuPopup* s_openMenu;
};
