#pragma once

#include "render/scene/input_dispatcher.h"
#include "render/animation/animation_manager.h"
#include "render/core/render_styles.h"
#include "shell/activity/transient_activity.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

class Bar;
class Node;
class PopupSurface;
class RenderContext;
class WaylandConnection;
struct PointerEvent;
struct wl_surface;
struct wl_output;
struct zwlr_layer_surface_v1;

struct TransientActivityAnchor {
  zwlr_layer_surface_v1* parent = nullptr;
  wl_output* output = nullptr;
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  std::string barPosition;
  RoundedRectStyle inheritedStyle;
};

class TransientActivityPopup {
public:
  TransientActivityPopup() = default;
  ~TransientActivityPopup();

  void initialize(WaylandConnection& wayland, RenderContext& renderContext, Bar& bar);
  void setUnavailableCallback(std::function<void()> callback) { m_unavailable = std::move(callback); }
  [[nodiscard]] bool canPresent(const TransientActivityRoute& route) const;
  [[nodiscard]] bool present(const TransientActivityViewModel& activity, const TransientActivityRoute& route);
  void withdraw(std::uint64_t serial);
  void withdrawImmediately(std::uint64_t serial);
  [[nodiscard]] bool onPointerEvent(const PointerEvent& event);

private:
  struct Instance {
    std::uint64_t serial = 0;
    TransientActivityMotion motion = TransientActivityMotion::Inherit;
    std::unique_ptr<PopupSurface> surface;
    AnimationManager animations;
    std::unique_ptr<Node> sceneRoot;
    InputDispatcher inputDispatcher;
  };

  void destroyAll();
  [[nodiscard]] std::unique_ptr<Instance>
  createInstance(const TransientActivityViewModel& activity, const TransientActivityRoute& route,
                 const TransientActivityAnchor& anchor);

  WaylandConnection* m_wayland = nullptr;
  RenderContext* m_renderContext = nullptr;
  Bar* m_bar = nullptr;
  std::function<void()> m_unavailable;
  std::vector<std::unique_ptr<Instance>> m_instances;
};
