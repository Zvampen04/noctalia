#include "shell/activity/transient_activity_popup.h"

#include "core/ui_phase.h"
#include "core/deferred_call.h"
#include "render/animation/motion_service.h"
#include "render/render_context.h"
#include "render/scene/input_area.h"
#include "shell/bar/bar.h"
#include "ui/builders.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include "wayland/wayland_connection.h"
#include "wayland/wayland_seat.h"
#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

namespace {

  TransientActivityPlacement resolvedPlacement(
      TransientActivityPlacement placement, const std::string& barPosition) {
    if (placement != TransientActivityPlacement::Auto) return placement;
    if (barPosition == "bottom") return TransientActivityPlacement::Above;
    if (barPosition == "left") return TransientActivityPlacement::Right;
    if (barPosition == "right") return TransientActivityPlacement::Left;
    return TransientActivityPlacement::Below;
  }

  PopupSurfaceConfig popupConfig(
      const TransientActivityRoute& route, const TransientActivityAnchor& source) {
    auto placement = resolvedPlacement(route.placement, source.barPosition);
    if (placement == TransientActivityPlacement::Before)
      placement = (source.barPosition == "left" || source.barPosition == "right")
          ? TransientActivityPlacement::Above : TransientActivityPlacement::Left;
    if (placement == TransientActivityPlacement::After)
      placement = (source.barPosition == "left" || source.barPosition == "right")
          ? TransientActivityPlacement::Below : TransientActivityPlacement::Right;

    PopupSurfaceConfig config{
        .anchorX = static_cast<std::int32_t>(std::lround(source.x)),
        .anchorY = static_cast<std::int32_t>(std::lround(source.y)),
        .anchorWidth = std::max(1, static_cast<std::int32_t>(std::lround(source.width))),
        .anchorHeight = std::max(1, static_cast<std::int32_t>(std::lround(source.height))),
        .width = static_cast<std::uint32_t>(route.width),
        .height = static_cast<std::uint32_t>(route.height),
        .constraintAdjustment = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X
            | XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y,
        .grab = false,
        .reactive = true,
    };
    const auto gap = static_cast<std::int32_t>(std::lround(Style::spaceSm));
    switch (placement) {
    case TransientActivityPlacement::Above:
      config.anchor = XDG_POSITIONER_ANCHOR_TOP;
      config.gravity = XDG_POSITIONER_GRAVITY_TOP;
      config.offsetY = -gap;
      break;
    case TransientActivityPlacement::Left:
      config.anchor = XDG_POSITIONER_ANCHOR_LEFT;
      config.gravity = XDG_POSITIONER_GRAVITY_LEFT;
      config.offsetX = -gap;
      break;
    case TransientActivityPlacement::Right:
      config.anchor = XDG_POSITIONER_ANCHOR_RIGHT;
      config.gravity = XDG_POSITIONER_GRAVITY_RIGHT;
      config.offsetX = gap;
      break;
    case TransientActivityPlacement::Below:
    default:
      config.anchor = XDG_POSITIONER_ANCHOR_BOTTOM;
      config.gravity = XDG_POSITIONER_GRAVITY_BOTTOM;
      config.offsetY = gap;
      break;
    }
    return config;
  }
}

TransientActivityPopup::~TransientActivityPopup() { destroyAll(); }

void TransientActivityPopup::initialize(WaylandConnection& wayland, RenderContext& renderContext, Bar& bar) {
  m_wayland = &wayland;
  m_renderContext = &renderContext;
  m_bar = &bar;
}

bool TransientActivityPopup::canPresent(const TransientActivityRoute& route) const {
  return m_wayland != nullptr && m_renderContext != nullptr && m_bar != nullptr
      && route.presentation == TransientActivityPresentation::Attached
      && !m_bar->transientActivityAnchors(route).empty();
}

bool TransientActivityPopup::present(
    const TransientActivityViewModel& activity, const TransientActivityRoute& route) {
  if (!canPresent(route)) return false;
  auto anchors = m_bar->transientActivityAnchors(route);
  std::vector<std::unique_ptr<Instance>> next;
  next.reserve(anchors.size());
  for (const auto& anchor : anchors) {
    auto instance = createInstance(activity, route, anchor);
    if (instance == nullptr) {
      next.clear();
      return false;
    }
    next.push_back(std::move(instance));
  }
  destroyAll();
  m_instances = std::move(next);
  return !m_instances.empty();
}

std::unique_ptr<TransientActivityPopup::Instance> TransientActivityPopup::createInstance(
    const TransientActivityViewModel& activity, const TransientActivityRoute& route,
    const TransientActivityAnchor& anchor) {
  if (anchor.parent == nullptr || anchor.output == nullptr) return nullptr;
  auto instance = std::make_unique<Instance>();
  instance->serial = activity.serial;
  instance->motion = route.motion;
  instance->surface = std::make_unique<PopupSurface>(*m_wayland);
  instance->surface->setRenderContext(m_renderContext);
  instance->surface->setDismissedCallback([this]() {
    DeferredCall::callLater([this]() {
      if (m_unavailable) m_unavailable();
    });
  });
  if (!instance->surface->initialize(anchor.parent, anchor.output, popupConfig(route, anchor))) return nullptr;

  UiPhaseScope layoutPhase(UiPhase::Layout);
  auto root = ui::inputArea({
      .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}),
      .width = static_cast<float>(route.width),
      .height = static_cast<float>(route.height),
      .onEnter = activity.hoverChanged
          ? std::function<void(const InputArea::PointerData&)>{[hover = activity.hoverChanged](const auto&) {
              hover(true);
            }} : std::function<void(const InputArea::PointerData&)>{},
      .onLeave = activity.hoverChanged
          ? std::function<void()>{[hover = activity.hoverChanged]() { hover(false); }} : std::function<void()>{},
      .onClick = [activate = activity.activate, dismiss = activity.dismiss](const InputArea::PointerData& data) {
        if (data.button == BTN_RIGHT && dismiss) dismiss();
        else if (data.button == BTN_LEFT && activate) activate();
      },
  });
  auto background = ui::box({
      .width = static_cast<float>(route.width),
      .height = static_cast<float>(route.height),
      .configure = [material = route.material, inherited = anchor.inheritedStyle](Box& box) {
        box.setMaterialIdentity("surface", "activity", "attached");
        if (material == TransientActivityMaterial::Transparent) box.setFill(clearColorSpec());
        else if (material == TransientActivityMaterial::Surface) box.setCardStyle();
        else box.setStyle(inherited);
      },
  });
  root->addChild(std::move(background));
  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm,
      .paddingH = Style::spaceMd,
      .width = static_cast<float>(route.width),
      .height = static_cast<float>(route.height),
  });
  row->addChild(ui::glyph({.glyph = activity.icon.empty() ? "bell" : activity.icon}));
  auto text = ui::column({.justify = FlexJustify::Center, .flexGrow = 1.0F});
  text->addChild(ui::label({
      .text = activity.title.empty() ? activity.value : activity.title,
      .maxLines = 1,
      .ellipsize = TextEllipsize::End,
      .flexGrow = 1.0F,
  }));
  if (route.showBody && !activity.body.empty()) text->addChild(ui::label({
      .text = activity.body, .fontSize = Style::fontSizeCaption, .maxLines = 1, .ellipsize = TextEllipsize::End}));
  row->addChild(std::move(text));
  if (!activity.title.empty() && !activity.value.empty()) row->addChild(ui::label({.text=activity.value,.maxLines=1}));
  if (activity.showProgress && activity.setProgress) {
    auto slider = std::make_unique<Slider>();
    slider->setRange(0.0, 1.0);
    slider->setStep(0.01);
    slider->setTrackHeight(static_cast<float>(route.progressThickness));
    slider->setValue(std::clamp(activity.progress, 0.0F, 1.0F));
    slider->setWheelAdjustEnabled(true);
    slider->setOnValueChanged([setProgress = activity.setProgress](double value) {
      setProgress(static_cast<float>(value));
    });
      slider->setMinWidth(std::clamp(route.width*.28F,24.F,96.F));
      slider->setMaxWidth(std::clamp(route.width*.28F,24.F,96.F));
    row->addChild(std::move(slider));
  } else if (activity.showProgress) {
    row->addChild(ui::progressBar({
        .progress = std::clamp(activity.progress, 0.0F, 1.0F),
        .width = 96.0F,
        .height = static_cast<float>(route.progressThickness),
    }));
  }
  root->addChild(std::move(row));
  instance->sceneRoot = std::move(root);
  if (route.motion == TransientActivityMotion::Inherit && MotionService::instance().enabled())
    instance->sceneRoot->setOpacity(0.0F);
  instance->inputDispatcher.setSceneRoot(instance->sceneRoot.get());
  instance->surface->setSceneRoot(instance->sceneRoot.get());
  instance->surface->setAnimationManager(&instance->animations);
  instance->surface->setInputRegion({{0, 0, static_cast<int>(route.width), static_cast<int>(route.height)}});
  instance->surface->setConfigureCallback([surface = instance->surface.get()](std::uint32_t, std::uint32_t) {
    surface->requestLayout();
  });
  instance->surface->setPrepareFrameCallback([this, raw = instance.get()](bool, bool) {
    if (raw->surface == nullptr || raw->sceneRoot == nullptr) return;
    m_renderContext->makeCurrent(raw->surface->renderTarget());
    UiPhaseScope phase(UiPhase::Layout);
    raw->sceneRoot->layout(raw->surface->renderTarget().renderer());
  });
  instance->surface->requestUpdate();
  if (route.motion == TransientActivityMotion::Inherit && MotionService::instance().enabled()) {
    instance->animations.animate(
        0.0F, 1.0F, Style::animFast, Easing::EaseOutCubic,
        [node = instance->sceneRoot.get()](float value) { node->setOpacity(value); }, {}, instance->sceneRoot.get());
  }
  return instance;
}

void TransientActivityPopup::withdraw(std::uint64_t serial) {
  if (serial == 0) {
    destroyAll();
    return;
  }
  for (auto& instance : m_instances) {
    if (instance == nullptr || instance->serial != serial) continue;
    if (instance->motion != TransientActivityMotion::Inherit || !MotionService::instance().enabled()
        || instance->sceneRoot == nullptr) {
      destroyAll();
      return;
    }
    auto* raw = instance.get();
    instance->animations.animate(
        instance->sceneRoot->opacity(), 0.0F, Style::animFast, Easing::EaseOutCubic,
        [node = instance->sceneRoot.get()](float value) { node->setOpacity(value); },
        [this, raw]() {
          DeferredCall::callLater([this, raw]() {
            std::erase_if(m_instances, [raw](const auto& value) { return value.get() == raw; });
          });
        }, instance->sceneRoot.get());
  }
}

void TransientActivityPopup::withdrawImmediately(std::uint64_t serial) {
  if (serial == 0 || std::ranges::any_of(m_instances, [serial](const auto& value) {
        return value != nullptr && value->serial == serial;
      })) destroyAll();
}

void TransientActivityPopup::destroyAll() {
  for (auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->inputDispatcher.setSceneRoot(nullptr);
      if (instance->surface != nullptr) instance->surface->setDismissedCallback(nullptr);
    }
  }
  m_instances.clear();
}

bool TransientActivityPopup::onPointerEvent(const PointerEvent& event) {
  auto found = std::ranges::find_if(m_instances, [&](const auto& instance) {
    return instance != nullptr && instance->surface != nullptr && instance->surface->wlSurface() == event.surface;
  });
  if (found == m_instances.end()) return false;
  auto& dispatcher = (*found)->inputDispatcher;
  const float x = static_cast<float>(event.sx);
  const float y = static_cast<float>(event.sy);
  switch (event.type) {
  case PointerEvent::Type::Enter: dispatcher.pointerEnter(x, y, event.serial); return true;
  case PointerEvent::Type::Leave: dispatcher.pointerLeave(); return true;
  case PointerEvent::Type::Motion: dispatcher.pointerMotion(x, y, event.serial); return true;
  case PointerEvent::Type::Button:
    return dispatcher.pointerButton(x, y, event.button, event.pressed, event.serial, event.time, event.touch);
  case PointerEvent::Type::Axis:
    return dispatcher.pointerAxis(x, y, event.axis, event.axisSource, event.axisValue, event.axisDiscrete,
                                  event.axisValue120, event.axisLines, event.axisGestureSerial);
  }
  return false;
}
