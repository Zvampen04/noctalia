#include "ui/controls/context_menu_popup.h"

#include "core/deferred_call.h"
#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "core/log.h"
#include "core/ui_phase.h"
#include "render/render_context.h"
#include "render/render_target.h"
#include "render/scene/node.h"
#include "render/scene/rect_node.h"
#include "ui/controls/box.h"
#include "ui/controls/scroll_view.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "ui/material_target_catalog.h"
#include "wayland/layer_surface.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("context-menu-popup");

} // namespace

ContextMenuPopup* ContextMenuPopup::s_openMenu = nullptr;

ContextMenuPopup::ContextMenuPopup(WaylandConnection& wayland, RenderContext& renderContext)
    : m_wayland(wayland), m_renderContext(renderContext) {
  const std::weak_ptr<bool> alive = m_alive;
  m_styleConnection = Style::surfaceMaterialChanged().connect([this, alive]() {
    const auto token = alive.lock();
    if (token && *token) refreshStyle(m_contentScale);
  });
}

ContextMenuPopup::~ContextMenuPopup() {
  if (m_alive != nullptr) {
    *m_alive = false;
  }
  close();
}

void ContextMenuPopup::open(ContextMenuPopupRequest request) {
  close();

  // maxVisible caps the popup viewport; all entries remain reachable via scroll.
  const std::size_t maxVisible =
      request.maxVisible > 0 ? request.maxVisible : std::max<std::size_t>(1, request.entries.size());
  const float contentScale = std::max(0.1F, request.contentScale);
  m_contentScale = contentScale;
  const float menuHeight = ContextMenuControl::preferredHeight(request.entries, maxVisible, contentScale);
  m_entries = std::make_shared<std::vector<ContextMenuControlEntry>>(request.entries);
  float menuWidth = request.menuWidth;
  if (menuWidth <= 0.0F) {
    float measureScale = 1.0F;
    if (request.parent.output != nullptr) {
      if (const WaylandOutput* out = m_wayland.findOutputByWl(request.parent.output); out != nullptr) {
        measureScale = out->configuredScale();
      }
    }
    ScaledRenderer measureRenderer(m_renderContext, measureScale);
    menuWidth = ContextMenuControl::preferredWidth(measureRenderer, request.entries, contentScale);
    if (request.maxMenuWidth > 0.0F) {
      menuWidth = std::min(menuWidth, request.maxMenuWidth);
    }
    if (request.minMenuWidth > 0.0F) {
      menuWidth = std::max(menuWidth, request.minMenuWidth);
    }
  }
  const auto chrome =
      popup_chrome::computeGeometry(menuWidth, menuHeight, m_shadowConfig, Style::popupShadowsEnabled(), request.parent.materialSurface,
          Style::controls().card_variant == Style::CardTreatment::Raised ? "card" : "container");
  m_scrollState = {};
  m_scrollView = nullptr;
  m_menu = nullptr;
  m_highlightedIndex = request.initialHighlight < request.entries.size() ? request.initialHighlight : 0;

  const ContextMenuPopupPlacement defaultPlacement{
      .anchor = XDG_POSITIONER_ANCHOR_BOTTOM,
      .gravity = XDG_POSITIONER_GRAVITY_BOTTOM,
      .offsetX = 0,
      .offsetY = static_cast<std::int32_t>(Style::spaceXs),
      .chromeAttachment = popup_chrome::Attachment{
          .horizontal = popup_chrome::HorizontalAttachment::Center, .vertical = popup_chrome::VerticalAttachment::Top
      },
  };
  const ContextMenuPopupPlacement resolvedPlacement = request.placement.value_or(defaultPlacement);

  PopupSurfaceConfig popupCfg{
      .anchorX = request.anchor.x,
      .anchorY = request.anchor.y,
      .anchorWidth = std::max(1, request.anchor.width),
      .anchorHeight = std::max(1, request.anchor.height),
      .width = chrome.surfaceWidth,
      .height = chrome.surfaceHeight,
      .anchor = resolvedPlacement.anchor,
      .gravity = resolvedPlacement.gravity,
      .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X
          | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y,
      .offsetX = resolvedPlacement.offsetX,
      .offsetY = resolvedPlacement.offsetY,
      .serial = request.inputSerial.value_or(m_wayland.lastInputSerial()),
      .grab = true,
  };
  popup_chrome::applyToConfig(popupCfg, chrome, resolvedPlacement.chromeAttachment);

  m_surface = std::make_unique<PopupSurface>(m_wayland);
  m_surface->setRenderContext(&m_renderContext);
  m_surface->setAnimationManager(&m_animations);

  auto* self = this;

  m_surface->setConfigureCallback([self](std::uint32_t /*w*/, std::uint32_t /*h*/) {
    self->m_surface->requestLayout();
  });

  auto liveChrome = std::make_shared<popup_chrome::Geometry>(chrome);
  auto liveScale = std::make_shared<float>(contentScale);
  m_refreshStyle = [this, liveChrome, liveScale, entries = m_entries, maxVisible, menuWidth,
                    automaticWidth = request.menuWidth <= 0, minWidth = request.minMenuWidth,
                    maxWidth = request.maxMenuWidth,
                    contentScale, popupCfg, resolvedPlacement, materialSurface = request.parent.materialSurface](float scale) mutable {
    if (!m_surface) return;
    scale = std::max(0.1F, scale);
    *liveScale = scale;
    float width = menuWidth * scale / contentScale;
    if (automaticWidth) {
      ScaledRenderer measureRenderer(m_renderContext, scale);
      width = ContextMenuControl::preferredWidth(measureRenderer, *entries, scale);
      if (maxWidth > 0) width = std::min(width, maxWidth * scale / contentScale);
      if (minWidth > 0) width = std::max(width, minWidth * scale / contentScale);
    }
    *liveChrome = popup_chrome::computeGeometry(width,
        ContextMenuControl::preferredHeight(*entries, maxVisible, scale), m_shadowConfig, Style::popupShadowsEnabled(), materialSurface,
        Style::controls().card_variant == Style::CardTreatment::Raised ? "card" : "container");
    popupCfg.width = liveChrome->surfaceWidth;
    popupCfg.height = liveChrome->surfaceHeight;
    popupCfg.offsetX = resolvedPlacement.offsetX;
    popupCfg.offsetY = resolvedPlacement.offsetY;
    popup_chrome::applyToConfig(popupCfg, *liveChrome, resolvedPlacement.chromeAttachment);
    m_surface->repositionAnchor(popupCfg, false);
    m_surface->resize(liveChrome->surfaceWidth, liveChrome->surfaceHeight, false);
    popup_chrome::setContentInputRegion(
        *m_surface, *liveChrome, Style::scaledRadiusLg(scale), Style::cornerPower);
    m_surface->requestLayout();
    m_surface->requestRedraw();
  };
  const auto materialSurfaces = Style::materialTargetSurfacePath(
      request.parent.materialSurface, m_materialClassTarget);
  m_surface->setPrepareFrameCallback([self, entries = m_entries, liveChrome,
                                      liveScale, materialSurface = request.parent.materialSurface,
                                      materialSurfaces](bool /*needsUpdate*/, bool needsLayout) {
    if (self->m_surface == nullptr) return;
    const bool styleChanged = self->applyPendingStyle();
    if (styleChanged) needsLayout = true;
    const auto& chrome = *liveChrome;
    const auto contentScale = *liveScale;

    const auto width = self->m_surface->width();
    const auto height = self->m_surface->height();
    if (width == 0 || height == 0) {
      return;
    }

    self->m_renderContext.makeCurrent(self->m_surface->renderTarget());

    const bool needsSceneBuild = self->m_sceneRoot == nullptr;
    if (!needsSceneBuild && !needsLayout) {
      return;
    }

    UiPhaseScope layoutPhase(UiPhase::Layout);

    if (self->m_menu != nullptr) {
      self->m_highlightedIndex = self->m_menu->highlightedIndex();
    }

    const auto fw = static_cast<float>(width);
    const auto fh = static_cast<float>(height);

    if (needsSceneBuild) {
      self->m_sceneRoot = std::make_unique<Node>();
      self->m_sceneRoot->setMaterialSurface(materialSurface);
      self->m_sceneRoot->setAnimationManager(&self->m_animations);
      auto transitionRoot = std::make_unique<Node>();
      transitionRoot->setClipChildren(true);
      self->m_transitionRoot = self->m_sceneRoot->addChild(std::move(transitionRoot));
      self->m_background = popup_chrome::addCardBackground(*self->m_transitionRoot, chrome, contentScale);
      self->m_background->setMaterialIdentityPath("surface", "container", materialSurfaces);

      auto scrollView = std::make_unique<ScrollView>();
      scrollView->setViewportPaddingH(0.0F);
      scrollView->setViewportPaddingV(0.0F);
      scrollView->clearFill();
      scrollView->clearBorder();
      scrollView->setRadius(0.0F);
      scrollView->bindState(&self->m_scrollState);
      scrollView->setScrollbarVisible(true);
      auto ctrl = std::make_unique<ContextMenuControl>();
      ctrl->setContentScale(contentScale);
      ctrl->setMenuWidth(chrome.contentWidth);
      ctrl->setMaxVisible(entries->size());
      ctrl->setEntries(*entries);
      ctrl->setHighlightedIndex(self->m_highlightedIndex);
      self->m_highlightedIndex = ctrl->highlightedIndex();
      ctrl->setRedrawCallback([self]() {
        if (self->m_surface) self->m_surface->requestRedraw();
      });
      ctrl->setOnActivate([self](const ContextMenuControlEntry& e) { self->deferActivation(e); });
      self->m_menu = ctrl.get();
      scrollView->content()->addChild(std::move(ctrl));
      self->m_scrollView = scrollView.get();
      self->m_transitionRoot->addChild(std::move(scrollView));
      self->m_inputDispatcher.setSceneRoot(self->m_sceneRoot.get());
      self->m_inputDispatcher.setCursorShapeCallback([self](std::uint32_t serial, std::uint32_t shape) {
        self->m_wayland.setCursorShape(serial, shape);
      });
      self->m_surface->setSceneRoot(self->m_sceneRoot.get());
    }

    // Keep menu, scroll view and input dispatcher identities across all layouts.
    self->m_sceneRoot->setSize(fw, fh);
    self->m_transitionRoot->setSize(fw, fh);
    if (styleChanged || needsSceneBuild) {
      if (self->m_panelShadow) {
        (void)self->m_transitionRoot->removeChild(self->m_panelShadow);
        self->m_panelShadow = nullptr;
      }
      if (Style::popupShadowsEnabled()) {
        self->m_panelShadow = popup_chrome::addShadow(*self->m_transitionRoot, chrome,
            self->m_shadowConfig, Style::scaledRadiusLg(contentScale));
      }
      self->m_background->setCardStyle(contentScale, 1.0F, Style::popupBordersEnabled());
      self->m_background->setRadius(Style::scaledRadiusLg(contentScale));
    }
    self->m_background->setPosition(chrome.contentX(), chrome.contentY());
    self->m_background->setSize(chrome.contentWidth, chrome.contentHeight);
    self->m_scrollView->setPosition(chrome.contentX(), chrome.contentY());
    self->m_scrollView->setSize(chrome.contentWidth, chrome.contentHeight);
    self->m_scrollView->setContentScale(contentScale);
    self->m_scrollView->setScrollbarInsetV(Style::scaledRadiusLg(contentScale));
    self->m_menu->setContentScale(contentScale);
    if (self->m_menu->width() != chrome.contentWidth) self->m_menu->setMenuWidth(chrome.contentWidth);
    self->m_scrollView->layout(self->m_surface->renderTarget().renderer());
    if (needsSceneBuild || std::exchange(self->m_revealHighlight, false)) self->ensureHighlightedVisible();
    if (needsSceneBuild) {
      self->m_transition.open(*self->m_transitionRoot, self->m_animations, [self]() {
        if (self->m_surface != nullptr) self->m_surface->requestRedraw();
      });
    }
  });

  m_surface->setDismissedCallback([self]() { self->deferClose(); });

  // Layer-shell popups inherit their parent's keyboard interactivity. A bar is
  // None, so flip it to OnDemand before the popup maps or the grabbing popup
  // gets no keyboard focus and ESC cannot reach it. Only bar-style callers pass
  // a parent wlSurface; panels are already OnDemand and leave it null.
  if (request.parent.layerSurface != nullptr && request.parent.wlSurface != nullptr) {
    m_keyboardParentLayerSurface = request.parent.layerSurface;
    m_keyboardParentWlSurface = request.parent.wlSurface;
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        request.parent.layerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::OnDemand)
    );
    wl_surface_commit(request.parent.wlSurface);
  }

  const bool initialized = request.parent.xdgSurface != nullptr
      ? m_surface->initializeAsChild(request.parent.xdgSurface, request.parent.output, popupCfg)
      : m_surface->initialize(request.parent.layerSurface, request.parent.output, popupCfg);
  if (!initialized) {
    kLog.warn("failed to create context menu popup");
    restoreParentKeyboardInteractivity();
    m_surface.reset();
    return;
  }

  popup_chrome::setContentInputRegion(
      *m_surface, chrome, Style::scaledRadiusLg(contentScale), Style::cornerPower);
  m_wlSurface = m_surface->wlSurface();
  m_pointerParentSurface =
      request.pointerParentSurface != nullptr ? request.pointerParentSurface : request.parent.wlSurface;
  s_openMenu = this;
  m_materialRegistration =
      Style::MaterialTargetCatalog::instance().registerClassTarget(m_materialClassTarget);
}

void ContextMenuPopup::close() {
  // A grabbing xdg_popup must be destroyed before an activation callback can
  // open its replacement. Retaining it for an exit fade would keep it topmost
  // and either add input latency or violate xdg_popup ordering, so menus use
  // the shared entrance transition and an immediate semantic/visual close.
  m_transition.reset();
  m_materialRegistration.reset();
  ++m_generation;
  m_entries.reset();
  m_refreshStyle = {};
  m_styleDirty = false;
  m_revealHighlight = false;
  m_background = nullptr;
  m_panelShadow = nullptr;
  m_transitionRoot = nullptr;
  const bool wasOpen = m_surface != nullptr;
  if (s_openMenu == this) {
    s_openMenu = nullptr;
  }
  restoreParentKeyboardInteractivity();
  m_menu = nullptr;
  m_scrollView = nullptr;
  m_inputDispatcher.setSceneRoot(nullptr);
  m_sceneRoot.reset();
  m_surface.reset();
  m_wlSurface = nullptr;
  m_pointerParentSurface = nullptr;
  m_pointerInside = false;
  if (wasOpen && m_onDismissed) {
    m_onDismissed();
  }
}

void ContextMenuPopup::setMaterialClassTarget(std::string_view target) {
  if (isOpen() || !Style::MaterialTargetCatalog::instance().supported(target)) return;
  m_materialClassTarget = target;
}

bool ContextMenuPopup::isOpen() const noexcept { return m_surface != nullptr; }

void ContextMenuPopup::deferActivation(ContextMenuControlEntry entry) {
  auto* self = this;
  const std::weak_ptr<bool> alive = m_alive;
  auto onActivate = m_onActivate;
  const auto generation = m_generation;
  DeferredCall::callLater([self, alive, generation, onActivate = std::move(onActivate), entry = std::move(entry)]() {
    const auto token = alive.lock();
    if (token == nullptr || !*token || generation != self->m_generation) {
      return;
    }
    // Close before running the action. The action may open another popup (e.g. the
    // color picker for a "Custom" entry); that popup must be created against the
    // now-topmost parent, and this menu cannot be destroyed while it still has a
    // child popup on top. Activating first violates the xdg_popup topmost rule.
    self->close();
    if (onActivate) {
      onActivate(entry);
    }
  });
}

void ContextMenuPopup::deferClose() {
  auto* self = this;
  const std::weak_ptr<bool> alive = m_alive;
  const auto generation = m_generation;
  DeferredCall::callLater([self, alive, generation]() {
    const auto token = alive.lock();
    if (token != nullptr && *token && generation == self->m_generation) {
      self->close();
    }
  });
}

void ContextMenuPopup::setOnActivate(std::function<void(const ContextMenuControlEntry&)> callback) {
  m_onActivate = std::move(callback);
}

void ContextMenuPopup::setOnDismissed(std::function<void()> callback) { m_onDismissed = std::move(callback); }

void ContextMenuPopup::setEntries(std::vector<ContextMenuControlEntry> entries) {
  if (!m_surface || !m_entries) return;
  const auto highlighted = m_menu ? m_menu->highlightedIndex() : m_highlightedIndex;
  std::optional<std::int32_t> selectedId;
  if (highlighted < m_entries->size()) selectedId = (*m_entries)[highlighted].id;
  ++m_generation;
  m_inputDispatcher.cancelPointerCapture();
  m_inputDispatcher.setSceneRoot(nullptr);
  *m_entries = std::move(entries);
  m_revealHighlight = true;
  m_highlightedIndex = 0;
  if (selectedId) {
    const auto found = std::ranges::find(*m_entries, *selectedId, &ContextMenuControlEntry::id);
    if (found != m_entries->end()) m_highlightedIndex = static_cast<std::size_t>(found - m_entries->begin());
  }
  if (m_menu) {
    m_menu->setMaxVisible(m_entries->size());
    m_menu->setEntries(*m_entries);
    m_menu->setHighlightedIndex(m_highlightedIndex);
    m_highlightedIndex = m_menu->highlightedIndex();
  }
  m_inputDispatcher.setSceneRoot(m_sceneRoot.get());
  refreshStyle(m_contentScale);
}

bool ContextMenuPopup::applyPendingStyle() {
  // A scale update can relayout menu rows. Finish an active press/scrollbar
  // drag before applying it so its captured input area remains alive.
  if (!m_surface || m_inputDispatcher.pointerCaptured() || !std::exchange(m_styleDirty, false)) return false;
  if (m_refreshStyle) m_refreshStyle(m_contentScale);
  return true;
}

void ContextMenuPopup::refreshStyle(float scale) {
  m_contentScale = std::max(0.1F, scale);
  m_styleDirty = true;
  if (m_surface) m_surface->requestLayout();
}

void ContextMenuPopup::setShadowConfig(const ShellConfig::ShadowConfig& shadow) {
  if (m_shadowConfig == shadow) {
    return;
  }
  m_shadowConfig = shadow;
  refreshStyle(m_contentScale);
}

bool ContextMenuPopup::onPointerEvent(const PointerEvent& event) {
  if (!isOpen()) {
    return false;
  }

  const bool captured = m_inputDispatcher.pointerCaptured();
  const bool onPopup = (event.surface != nullptr && event.surface == m_wlSurface);
  auto localX = static_cast<float>(event.sx);
  auto localY = static_cast<float>(event.sy);
  // While a press holds the pointer capture (e.g. a scrollbar thumb drag), events sliding onto the
  // parent surface are translated into popup-local coordinates so the drag keeps tracking instead
  // of being cut off at the popup edge.
  bool mapped = onPopup;
  if (!onPopup
      && captured
      && m_surface != nullptr
      && event.surface != nullptr
      && event.surface == m_pointerParentSurface) {
    localX -= static_cast<float>(m_surface->configuredX());
    localY -= static_cast<float>(m_surface->configuredY());
    mapped = true;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter:
    if (onPopup) {
      m_pointerInside = true;
      m_inputDispatcher.pointerEnter(localX, localY, event.serial);
    } else if (mapped) {
      // Parent-surface enter during a captured drag: swallow it so the parent's hover
      // states don't react while the drag owns the pointer.
      return true;
    }
    break;
  case PointerEvent::Type::Leave:
    if (onPopup) {
      m_pointerInside = false;
      // A captured drag survives leaving the surface; the deferred leave is delivered on release.
      if (!captured) {
        m_inputDispatcher.pointerLeave();
      }
    }
    break;
  case PointerEvent::Type::Motion:
    if (mapped || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      m_inputDispatcher.pointerMotion(localX, localY, 0);
      return true;
    }
    break;
  case PointerEvent::Type::Button:
    if (mapped || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      const bool pressed = event.pressed;
      m_inputDispatcher.pointerButton(localX, localY, event.button, pressed, event.serial, event.time, event.touch);
      if (!pressed && captured && !onPopup) {
        m_pointerInside = false;
        m_inputDispatcher.pointerLeave();
      }
      requestVisualUpdate();
      return true;
    }
    break;
  case PointerEvent::Type::Axis:
    if (onPopup || m_pointerInside) {
      if (onPopup) {
        m_pointerInside = true;
      }
      const bool consumed = m_inputDispatcher.pointerAxis(
          localX, localY, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120,
          event.axisLines
      );
      if (m_surface != nullptr && m_sceneRoot != nullptr) {
        if (m_sceneRoot->layoutDirty()) {
          m_surface->requestLayout();
        } else if (m_sceneRoot->paintDirty() || consumed) {
          m_surface->requestRedraw();
        }
      }
      return consumed || onPopup;
    }
    break;
  }

  if (m_surface != nullptr && m_sceneRoot != nullptr && m_surface->isRunning()) {
    m_surface->requestRedraw();
  }

  return onPopup;
}

void ContextMenuPopup::onKeyboardEvent(const KeyboardEvent& event) {
  if (!isOpen() || !event.pressed || event.preedit) {
    return;
  }

  const std::uint32_t sym = event.sym;
  const std::uint32_t modifiers = event.modifiers;

  if (KeybindMatcher::matches(KeybindAction::Cancel, sym, modifiers)) {
    deferClose();
    return;
  }

  if (m_menu == nullptr) {
    return;
  }

  if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
    if (m_menu->moveHighlight(1)) {
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  } else if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
    if (m_menu->moveHighlight(-1)) {
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  } else if (KeybindMatcher::matches(KeybindAction::Validate, sym, modifiers)) {
    (void)m_menu->activateHighlighted();
  } else if (KeySymbol::isHome(sym)) {
    m_menu->setHighlightedIndex(0);
    m_highlightedIndex = m_menu->highlightedIndex();
    ensureHighlightedVisible();
    requestVisualUpdate();
  } else if (KeySymbol::isEnd(sym)) {
    if (m_menu->entryCount() > 0) {
      m_menu->setHighlightedIndex(m_menu->entryCount() - 1);
      m_highlightedIndex = m_menu->highlightedIndex();
      ensureHighlightedVisible();
      requestVisualUpdate();
    }
  }
}

void ContextMenuPopup::ensureHighlightedVisible() {
  if (m_menu == nullptr || m_scrollView == nullptr || !m_scrollView->scrollable()) {
    return;
  }
  const std::size_t index = m_menu->highlightedIndex();
  const float rowTop = m_menu->rowTop(index);
  const float rowBottom = m_menu->rowBottom(index);
  const float viewportH = m_scrollView->contentViewportHeight();
  const float offset = m_scrollView->scrollOffset();
  if (rowTop < offset) {
    m_scrollView->setScrollOffset(rowTop);
  } else if (rowBottom > offset + viewportH) {
    m_scrollView->setScrollOffset(rowBottom - viewportH);
  }
}

void ContextMenuPopup::requestVisualUpdate() {
  if (m_surface == nullptr || m_sceneRoot == nullptr) {
    return;
  }
  if (m_sceneRoot->layoutDirty() || (m_styleDirty && !m_inputDispatcher.pointerCaptured())) {
    m_surface->requestLayout();
  } else {
    m_surface->requestRedraw();
  }
}

void ContextMenuPopup::restoreParentKeyboardInteractivity() {
  if (m_keyboardParentLayerSurface == nullptr) {
    return;
  }
  zwlr_layer_surface_v1_set_keyboard_interactivity(
      m_keyboardParentLayerSurface, static_cast<std::uint32_t>(LayerShellKeyboard::None)
  );
  if (m_keyboardParentWlSurface != nullptr) {
    wl_surface_commit(m_keyboardParentWlSurface);
  }
  m_keyboardParentLayerSurface = nullptr;
  m_keyboardParentWlSurface = nullptr;
}

bool ContextMenuPopup::dispatchKeyboardEvent(const KeyboardEvent& event) {
  if (s_openMenu == nullptr || !s_openMenu->isOpen()) {
    return false;
  }
  s_openMenu->onKeyboardEvent(event);
  return true;
}

wl_surface* ContextMenuPopup::wlSurface() const noexcept { return m_wlSurface; }

xdg_surface* ContextMenuPopup::xdgSurface() const noexcept {
  return m_surface != nullptr ? m_surface->xdgSurface() : nullptr;
}

std::uint32_t ContextMenuPopup::width() const noexcept { return m_surface != nullptr ? m_surface->width() : 0; }

std::uint32_t ContextMenuPopup::height() const noexcept { return m_surface != nullptr ? m_surface->height() : 0; }
