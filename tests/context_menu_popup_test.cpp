#include "core/deferred_call.h"
#include "render/render_context.h"
#include "shell/panel/panel.h"
#include "shell/panel/panel_manager.h"
#include "tests/test_check.h"
#include "ui/controls/context_menu_popup.h"
#include "wayland/hyprland/focus_grab_service.h"
#include "wayland/hyprland/popup_grab_host.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"

#include <cstdint>
#include <memory>

class ContextMenuPopupTestAccess {
public:
  static void activate(ContextMenuPopup& popup, ContextMenuControlEntry entry) {
    popup.deferActivation(std::move(entry));
  }

  static void dismiss(ContextMenuPopup& popup) { popup.deferClose(); }
};

class WaylandConnectionTestAccess {
public:
  static FocusGrabService& installFocusGrabService(WaylandConnection& wayland) {
    wayland.m_focusGrabService = std::make_unique<FocusGrabService>();
    return *wayland.m_focusGrabService;
  }
};

class PopupSurfaceTestAccess {
public:
  static void wireGrab(PopupSurface& popup, wl_surface* surface) {
    popup.m_surface = surface;
    popup.wireGrab();
  }

  static void clearSurface(PopupSurface& popup) { popup.m_surface = nullptr; }
};

namespace {

  class RecordingPopupGrabHost final : public PopupGrabHost {
  public:
    void registerPopupSurface(wl_surface* surface) override {
      ++registrations;
      registeredSurface = surface;
    }

    void unregisterPopupSurface(wl_surface* /*surface*/) override { ++unregistrations; }

    int registrations = 0;
    int unregistrations = 0;
    wl_surface* registeredSurface = nullptr;
  };

  class PopupOwningPanel final : public Panel {
  public:
    PopupOwningPanel(WaylandConnection& wayland, RenderContext& renderContext, int& activations)
        : m_popup(wayland, renderContext) {
      m_popup.setOnActivate([&activations](const ContextMenuControlEntry&) { ++activations; });
    }

    void create() override {}
    [[nodiscard]] float preferredWidth() const override { return 1.0F; }
    [[nodiscard]] float preferredHeight() const override { return 1.0F; }

    void deferActivation() {
      ContextMenuPopupTestAccess::activate(m_popup, ContextMenuControlEntry{.id = 2, .label = "Delete"});
    }

  protected:
    void doLayout(Renderer&, float, float) override {}

  private:
    ContextMenuPopup m_popup;
  };

  void drainDeferredCalls() {
    for (auto& callback : DeferredCall::takePending()) {
      callback();
    }
  }

} // namespace

int main() {
  WaylandConnection wayland;
  RenderContext renderContext;

  int activations = 0;
  {
    ContextMenuPopup popup(wayland, renderContext);
    popup.setOnActivate([&activations](const ContextMenuControlEntry&) { ++activations; });
    ContextMenuPopupTestAccess::activate(popup, ContextMenuControlEntry{.id = 1, .label = "Copy"});
    drainDeferredCalls();
  }
  TEST_CHECK(activations == 1);

  // Plugin unregistration destroys its panel-owned popup immediately. A queued
  // activation must then become a no-op instead of dereferencing the destroyed
  // ContextMenuPopup on the next main-loop iteration.
  {
    PanelManager panels;
    auto panel = std::make_unique<PopupOwningPanel>(wayland, renderContext, activations);
    panel->deferActivation();
    panels.registerPanel("test/plugin:panel", std::move(panel));
    panels.unregisterPanel("test/plugin:panel");
  }
  drainDeferredCalls();
  TEST_CHECK(activations == 1);

  // A Hyprland focus grab only accepts mapped surfaces. PopupSurface wires its
  // role before the first map, so host enrollment must wait for the next main
  // loop turn instead of being committed synchronously during initialization.
  {
    WaylandConnection popupWayland;
    auto& focusGrabService = WaylandConnectionTestAccess::installFocusGrabService(popupWayland);
    RecordingPopupGrabHost grabHost;
    focusGrabService.setPopupGrabHost(&grabHost);

    PopupSurface popup(popupWayland);
    auto* fakeSurface = reinterpret_cast<wl_surface*>(static_cast<std::uintptr_t>(1));
    PopupSurfaceTestAccess::wireGrab(popup, fakeSurface);
    TEST_CHECK(grabHost.registrations == 0);

    drainDeferredCalls();
    TEST_CHECK(grabHost.registrations == 1);
    TEST_CHECK(grabHost.registeredSurface == fakeSurface);

    // Do not let the test double reach the real Wayland destroy path.
    focusGrabService.setPopupGrabHost(nullptr);
    PopupSurfaceTestAccess::clearSurface(popup);
  }

  return 0;
}
