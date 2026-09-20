#include "shell/bar/bar.h"

#include "compositors/compositor_platform.h"
#include "config/config_service.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "core/process/process.h"
#include "core/scoped_timer.h"
#include "core/timer_manager.h"
#include "core/ui_phase.h"
#include "idle/idle_inhibitor.h"
#include "ipc/ipc_arg_parse.h"
#include "ipc/ipc_service.h"
#include "render/render_context.h"
#include "render/animation/motion_service.h"
#include "render/scene/input_area.h"
#include "shell/bar/bar_corner_shape.h"
#include "shell/bar/bar_reserved_zone.h"
#include "shell/bar/bar_island_morph_geometry.h"
#include "shell/bar/bar_section_geometry.h"
#include "shell/bar/bar_dynamic_section_geometry.h"
#include "shell/bar/widget.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/bar/widgets/plugin_widget.h"
#include "shell/bar/widgets/taskbar_widget.h"
#include "shell/bar/widgets/tray_widget.h"
#include "shell/activity/transient_activity.h"
#include "shell/activity/transient_activity_popup.h"
#include "shell/bar/bar_material_target.h"
#include "shell/panel/panel_manager.h"
#include "shell/surface/shadow.h"
#include "shell/tooltip/tooltip_manager.h"
#include "ui/builders.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <linux/input-event-codes.h>
#include <optional>
#include <ranges>
#include <unordered_set>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

namespace {

  constexpr Logger kLog("bar");
  constexpr std::chrono::milliseconds kWorkspaceRevealDebounce{80};
  constexpr std::chrono::milliseconds kWorkspacePeekHold{450};
  constexpr std::int32_t kAutoHideTriggerPx = 3;
  constexpr float kAutoHideSlideExtraPx = 4.0F;

  [[nodiscard]] SegmentContourKind segmentContourKind(BarCapsuleContour contour) noexcept {
    switch (contour) {
    case BarCapsuleContour::Powerline: return SegmentContourKind::Powerline;
    case BarCapsuleContour::PowerlineStart: return SegmentContourKind::PowerlineStart;
    case BarCapsuleContour::PowerlineEnd: return SegmentContourKind::PowerlineEnd;
    case BarCapsuleContour::Rounded: return SegmentContourKind::None;
    }
    return SegmentContourKind::None;
  }

  [[nodiscard]] SegmentContour segmentContourFor(const WidgetBarCapsuleSpec& spec, bool vertical) noexcept {
    return {.kind = segmentContourKind(spec.contour), .depth = spec.contourDepth, .vertical = vertical};
  }

  [[nodiscard]] std::string activeWorkspaceId(const std::vector<Workspace>& workspaces) {
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        if (!workspace.id.empty()) {
          return workspace.id;
        }
        if (!workspace.name.empty()) {
          return workspace.name;
        }
        return std::to_string(workspace.index);
      }
    }
    return {};
  }

  [[nodiscard]] bool barConfigUsesSlideSurface(const BarConfig& cfg) noexcept { return cfg.isAutoHideEnabled(); }

  [[nodiscard]] bool barSupportsSlideBehavior(const BarConfig& cfg) noexcept { return cfg.isAutoHideEnabled(); }

  [[nodiscard]] bool barPointerHideAllowed(const BarInstance& instance) noexcept {
    if (instance.barConfig.smartAutoHide) {
      return !instance.smartAutoHidePinnedVisible;
    }
    return instance.barConfig.autoHide;
  }

  [[nodiscard]] bool workspaceKeyMatchesAssignment(std::string_view assignmentKey, const Workspace& workspace) {
    if (assignmentKey.empty()) {
      return false;
    }
    if (!workspace.id.empty() && assignmentKey == workspace.id) {
      return true;
    }
    if (!workspace.name.empty() && assignmentKey == workspace.name) {
      return true;
    }
    if (workspace.index > 0 && assignmentKey == std::to_string(workspace.index)) {
      return true;
    }
    return false;
  }

  [[nodiscard]] bool activeWorkspaceHasWindows(const CompositorPlatform& platform, wl_output* output) {
    const auto workspaces = platform.workspaces(output);
    const Workspace* active = nullptr;
    for (const auto& workspace : workspaces) {
      if (workspace.active) {
        active = &workspace;
        break;
      }
    }
    if (active == nullptr) {
      return false;
    }

    const auto assignments = platform.workspaceWindowAssignments(output);
    for (const auto& assignment : assignments) {
      if (workspaceKeyMatchesAssignment(assignment.workspaceKey, *active)) {
        return true;
      }
    }
    if (!assignments.empty()) {
      return false;
    }
    return active->occupied;
  }

  [[nodiscard]] bool smartAutoHideWantsPinnedVisible(const CompositorPlatform& platform, wl_output* output) {
    if (platform.hasOverviewState() && platform.isOverviewOpen()) {
      return true;
    }
    return !activeWorkspaceHasWindows(platform, output);
  }

  [[nodiscard]] int barAutoHideEdgeGutter(const BarConfig& cfg) noexcept {
    if (!barConfigUsesSlideSurface(cfg) || cfg.marginEdge <= 0) {
      return 0;
    }
    return cfg.marginEdge;
  }

  [[nodiscard]] std::vector<InputRect>
  barAutoHideSurfaceInputRegion(const BarConfig& cfg, int surfW, int surfH, bool fullSurface) {
    if (surfW <= 0 || surfH <= 0) {
      return {};
    }
    if (fullSurface) {
      return {InputRect{0, 0, surfW, surfH}};
    }

    const int strip = std::min(kAutoHideTriggerPx, cfg.position == "left" || cfg.position == "right" ? surfW : surfH);
    if (cfg.position == "bottom") {
      return {InputRect{0, surfH - strip, surfW, strip}};
    }
    if (cfg.position == "left") {
      return {InputRect{0, 0, strip, surfH}};
    }
    if (cfg.position == "right") {
      return {InputRect{surfW - strip, 0, strip, surfH}};
    }
    return {InputRect{0, 0, surfW, strip}};
  }

  bool pointInsideNode(const Node* node, float sceneX, float sceneY) {
    if (node == nullptr) {
      return false;
    }
    float localX = 0.0F;
    float localY = 0.0F;
    if (!Node::mapFromScene(node, sceneX, sceneY, localX, localY)) {
      return false;
    }
    return localX >= 0.0F && localX < node->width() && localY >= 0.0F && localY < node->height();
  }

  HitTestOutset crossAxisOutsetToSlot(const Node* node, const Node* slot, bool isVertical) {
    if (node == nullptr || slot == nullptr) {
      return {};
    }

    float nodeX = 0.0F;
    float nodeY = 0.0F;
    float slotX = 0.0F;
    float slotY = 0.0F;
    Node::absolutePosition(node, nodeX, nodeY);
    Node::absolutePosition(slot, slotX, slotY);

    if (isVertical) {
      return {
          .left = std::max(0.0F, nodeX - slotX),
          .top = 0.0F,
          .right = std::max(0.0F, (slotX + slot->width()) - (nodeX + node->width())),
          .bottom = 0.0F,
      };
    }

    return {
        .left = 0.0F,
        .top = std::max(0.0F, nodeY - slotY),
        .right = 0.0F,
        .bottom = std::max(0.0F, (slotY + slot->height()) - (nodeY + node->height())),
    };
  }

  void applyBarWidgetHitTargets(Node* node, const Node* slot, bool isVertical, float stableCrossEnvelope = 0.0F) {
    if (node == nullptr || slot == nullptr) {
      return;
    }

    if (dynamic_cast<InputArea*>(node) != nullptr || node->clipChildren()) {
      auto outset = crossAxisOutsetToSlot(node, slot, isVertical);
      // Hover paint can translate away from the pointer. Keep input over the union of the compact
      // and fully-grown positions so entering at the edge cannot repeatedly leave/re-enter while
      // the island animates.
      if (isVertical) {
        outset.left += stableCrossEnvelope;
        outset.right += stableCrossEnvelope;
      } else {
        outset.top += stableCrossEnvelope;
        outset.bottom += stableCrossEnvelope;
      }
      node->setHitTestOutset(outset);
    }

    for (const auto& child : node->children()) {
      applyBarWidgetHitTargets(child.get(), slot, isVertical, stableCrossEnvelope);
    }
  }

  Widget* widgetAtPoint(const std::vector<std::unique_ptr<Widget>>& widgets, float sceneX, float sceneY) {
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr
          || widget->isBarClickThrough()
          || widget->outerNode() == nullptr
          || !widget->outerNode()->visible()) {
        continue;
      }
      // Bounds only, never Node::hitTest: hit outsets deliberately extend a widget's clickable
      // band past its ink (bar.cpp applies the hover pill padding that way), and treating that
      // band as "on a widget" would carve unreachable holes out of the dead zone.
      if (pointInsideNode(widget->outerNode(), sceneX, sceneY)) {
        return widget;
      }
    }
    for (const auto& widgetPtr : std::views::reverse(widgets)) {
      auto* widget = widgetPtr.get();
      if (widget == nullptr || widget->isBarClickThrough()) {
        continue;
      }
      auto* root = widget != nullptr ? widget->outerNode() : nullptr;
      auto* bounds = widget != nullptr ? widget->layoutBoundsNode() : nullptr;
      if (root == nullptr || bounds == nullptr || bounds == root || root->parent() != bounds || !bounds->visible()) {
        continue;
      }
      if (pointInsideNode(bounds, sceneX, sceneY)) {
        return widget;
      }
    }
    return nullptr;
  }

  Widget* widgetAtPoint(const BarInstance& instance, float sceneX, float sceneY) {
    for (const auto& section : std::views::reverse(instance.dynamicSections)) {
      if (auto* widget = widgetAtPoint(section.widgets, sceneX, sceneY); widget != nullptr) {
        return widget;
      }
    }
    if (auto* widget = widgetAtPoint(instance.endWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    if (auto* widget = widgetAtPoint(instance.centerWidgets, sceneX, sceneY); widget != nullptr) {
      return widget;
    }
    return widgetAtPoint(instance.startWidgets, sceneX, sceneY);
  }

  std::pair<float, float> surfaceOriginForOutputLocal(const BarInstance& instance, const WaylandOutput& outputInfo) {
    if (instance.surface == nullptr) {
      return {0.0F, 0.0F};
    }
    const auto* surface = instance.surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const auto mTop = static_cast<float>(surface->marginTop());
    const auto mRight = static_cast<float>(surface->marginRight());
    const auto mBottom = static_cast<float>(surface->marginBottom());
    const auto mLeft = static_cast<float>(surface->marginLeft());
    const auto surfW = static_cast<float>(surface->width());
    const auto surfH = static_cast<float>(surface->height());
    const auto outputW = static_cast<float>(outputInfo.effectiveLogicalWidth());
    const auto outputH = static_cast<float>(outputInfo.effectiveLogicalHeight());

    float x = 0.0F;
    float y = 0.0F;
    if (aLeft && aRight) {
      x = mLeft;
    } else if (aRight) {
      x = std::max(0.0F, outputW - mRight - surfW);
    } else {
      x = mLeft;
    }

    if (aTop && aBottom) {
      y = mTop;
    } else if (aBottom) {
      y = std::max(0.0F, outputH - mBottom - surfH);
    } else {
      y = mTop;
    }
    return {x, y};
  }

  bool isBarDeadZone(const BarInstance& instance, float sceneX, float sceneY) {
    if (widgetAtPoint(instance, sceneX, sceneY) != nullptr) {
      return false;
    }
    if (instance.barConfig.sectionBackgrounds) {
      if (!instance.dynamicSections.empty()) {
        return std::ranges::any_of(instance.dynamicSections, [&](const DynamicBarSection& section) {
          const auto* envelope = section.inputEnvelope != nullptr ? section.inputEnvelope : section.background;
          return envelope != nullptr && envelope->visible() && pointInsideNode(envelope, sceneX, sceneY);
        });
      }
      return std::ranges::any_of(instance.sectionBackgrounds, [&](const Box* background) {
        return background != nullptr && background->visible() && pointInsideNode(background, sceneX, sceneY);
      });
    }
    return pointInsideNode(instance.startSection, sceneX, sceneY)
        || pointInsideNode(instance.centerSection, sceneX, sceneY)
        || pointInsideNode(instance.endSection, sceneX, sceneY)
        || pointInsideNode(instance.sceneRoot.get(), sceneX, sceneY);
  }

  // Bar tooltips stay suppressed while any panel opened from the bar is alive.
  void suppressTooltipForOpenPanel(std::string_view panelId) {
    TooltipManager::instance().suppressBarTooltipsForPanel(panelId);
  }

  // The dead zone has no widget to anchor to, so a panel action anchors at the pointer instead.
  void openPanelAtBarPointer(
      BarInstance& instance, float sx, float sy, CompositorPlatform* platform, std::string_view sourceBarName,
      std::string_view panelId, std::string_view context, bool toggle
  ) {
    auto& panelManager = PanelManager::instance();
    if (toggle && panelManager.isOpenPanel(std::string(panelId))) {
      panelManager.closePanel();
      return;
    }

    float anchorX = sx;
    float anchorY = sy;
    if (platform != nullptr && instance.output != nullptr) {
      if (const auto* out = platform->findOutputByWl(instance.output); out != nullptr && out->hasUsableGeometry()) {
        const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(instance, *out);
        anchorX += surfaceX;
        anchorY += surfaceY;
      }
    }
    panelManager.openPanel(
        std::string(panelId),
        PanelOpenRequest{
            .output = instance.output,
            .anchorX = anchorX,
            .anchorY = anchorY,
            .hasAnchorPosition = true,
            .context = std::string(context),
            .sourceBarName = std::string(sourceBarName),
        }
    );
  }

  bool dispatchBarDeadZoneGesture(
      BarInstance& instance, noctalia::bar::Gesture gesture, float sx, float sy, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    const std::array<const Node*, 3> sections{
        instance.startSection, instance.centerSection, instance.endSection};
    const noctalia::bar::WidgetActionBindings* bindings = &instance.deadZoneBindings;
    for (const auto& section : std::views::reverse(instance.dynamicSections)) {
      const Node* target = instance.barConfig.sectionBackgrounds
          ? static_cast<const Node*>(section.inputEnvelope != nullptr ? section.inputEnvelope : section.background)
          : static_cast<const Node*>(section.content);
      if (target != nullptr && target->visible() && pointInsideNode(target, sx, sy)
          && section.bindings.find(gesture) != nullptr) {
        bindings = &section.bindings;
        break;
      }
    }
    for (std::size_t i = 0; i < sections.size(); ++i) {
      const Node* target = instance.barConfig.sectionBackgrounds
          ? static_cast<const Node*>(instance.sectionBackgrounds[i])
          : sections[i];
      if (target != nullptr && target->visible() && pointInsideNode(target, sx, sy)
          && instance.laneBindings[i].find(gesture) != nullptr) {
        bindings = &instance.laneBindings[i];
        break;
      }
    }
    const auto* action = bindings->find(gesture);
    if (action == nullptr) {
      return false;
    }

    if (action->kind == noctalia::bar::WidgetAction::Kind::Ipc && noctalia::bar::isAnchoredPanelVerb(action->verb)) {
      const auto args = noctalia::bar::parsePanelVerbArgs(action->args);
      if (args.panelId.empty()) {
        kLog.error(
            "bar.{}.dead_zone.actions.{}: \"{}\" needs a panel id", instance.barConfig.name, gestureConfigKey(gesture),
            action->verb
        );
        return false;
      }
      openPanelAtBarPointer(
          instance, sx, sy, platform, instance.barConfig.name, args.panelId, args.panelContext,
          action->verb == "panel-toggle"
      );
      return true;
    }

    // The dispatcher reports the command and the failure itself; repeating it here would double
    // every line on a held scroll.
    return dispatcher.run(*action, IpcInvocationContext{.barName = instance.barConfig.name, .output = instance.output});
  }

  bool handleBarDeadZoneButton(
      BarInstance& instance, float sx, float sy, std::uint32_t button, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }
    const auto gesture = noctalia::bar::gestureForButton(button);
    if (!gesture.has_value()) {
      return false;
    }
    return dispatchBarDeadZoneGesture(instance, *gesture, sx, sy, platform, dispatcher);
  }

  bool handleBarDeadZoneAxis(
      BarInstance& instance, float sx, float sy, const PointerEvent& event, CompositorPlatform* platform,
      const noctalia::bar::WidgetActionDispatcher& dispatcher
  ) {
    if (!isBarDeadZone(instance, sx, sy)) {
      return false;
    }

    // Routed through a scene-less InputArea for detent accumulation. Cycle actions fire once per
    // gesture; other actions fire for every step, matching a widget's `scroll_repeat = "auto"`.
    instance.deadZoneAxisSink.setOnAxisHandler([&](const InputArea::PointerData& data) {
      const auto gesture = noctalia::bar::gestureForScroll(data.axis, data.scrollSteps());
      if (!gesture.has_value()) {
        return false;
      }
      const auto* action = instance.deadZoneBindings.find(*gesture);
      for (const auto& section : std::views::reverse(instance.dynamicSections)) {
        const Node* target = instance.barConfig.sectionBackgrounds
            ? static_cast<const Node*>(section.inputEnvelope != nullptr ? section.inputEnvelope : section.background)
            : static_cast<const Node*>(section.content);
        if (target != nullptr && target->visible() && pointInsideNode(target, sx, sy)
            && section.bindings.find(*gesture) != nullptr) {
          action = section.bindings.find(*gesture);
          break;
        }
      }
      const std::array<const Node*, 3> sections{
          instance.startSection, instance.centerSection, instance.endSection};
      for (std::size_t i = 0; i < sections.size(); ++i) {
        const Node* target = instance.barConfig.sectionBackgrounds
            ? static_cast<const Node*>(instance.sectionBackgrounds[i])
            : sections[i];
        if (target != nullptr && target->visible() && pointInsideNode(target, sx, sy)
            && instance.laneBindings[i].find(*gesture) != nullptr) {
          action = instance.laneBindings[i].find(*gesture);
          break;
        }
      }
      if (!data.scrollStepStartsGesture() && action != nullptr && dispatcher.cycles(*action)) {
        return true;
      }
      return dispatchBarDeadZoneGesture(instance, *gesture, sx, sy, platform, dispatcher);
    });
    return instance.deadZoneAxisSink.dispatchAxis(
        sx, sy, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
  }

  std::uint32_t positionToAnchor(const std::string& position) {
    if (position == "bottom") {
      return LayerShellAnchor::Bottom | LayerShellAnchor::Left | LayerShellAnchor::Right;
    }
    if (position == "left") {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Left;
    }
    if (position == "right") {
      return LayerShellAnchor::Top | LayerShellAnchor::Bottom | LayerShellAnchor::Right;
    }
    // Default: top
    return LayerShellAnchor::Top | LayerShellAnchor::Left | LayerShellAnchor::Right;
  }

  // Hover highlight: peak fill alpha of the widget-foreground tint, and the cross-axis inset
  // (logical px, content-scaled) of per-member pills inside capsule groups.
  constexpr float kWidgetHoverFillAlpha = 0.1F;
  constexpr float kGroupHoverCrossInset = 2.0F;

  // Sizes a run's hover overlays after the capsule geometry is final. Single runs (and plain
  // widgets) get one overlay covering the whole shell; group runs get one pill per member so
  // only the hovered member lights up.
  void placeCapsuleHoverBoxes(
      BarCapsuleRun& run, bool isVertical, float shellW, float shellH, float contentX, float contentY,
      float capsuleRadius, float widgetHoverPadding
  ) {
    if (run.hoverBoxes.empty()) {
      return;
    }
    if (run.container == nullptr) {
      Box* box = run.hoverBoxes.front();
      if (box == nullptr) {
        return;
      }
      box->setPosition(0.0F, 0.0F);
      box->setSize(shellW, shellH);
      box->setRadius(capsuleRadius);
      return;
    }
    const float scale = run.contentScale;
    const float crossInset = kGroupHoverCrossInset * scale;
    // Use the normal per-widget inset, independent of the shared capsule's configurable
    // padding, so grouped and standalone widgets receive the same hover treatment.
    const float mainInset = widgetHoverPadding * scale;
    const float shellMain = isVertical ? shellH : shellW;
    const float shellCross = isVertical ? shellW : shellH;
    const float contentMain = isVertical ? contentY : contentX;
    const float crossExtent = std::max(0.0F, shellCross - 2.0F * crossInset);
    for (std::size_t i = 0; i < run.widgets.size() && i < run.hoverBoxes.size(); ++i) {
      // outerNode(), not root(): the outer gesture area is what the run container lays out, so it
      // carries the member's position; root() sits at (0,0) inside it.
      Node* member = run.widgets[i] != nullptr ? run.widgets[i]->outerNode() : nullptr;
      Box* box = run.hoverBoxes[i];
      if (member == nullptr || box == nullptr) {
        continue;
      }
      // Skip hidden members — Flex leaves them at stale (0,0) geometry.
      if (!member->visible() || !member->participatesInLayout()) {
        box->setSize(0.0F, 0.0F);
        continue;
      }
      const float rootStart = contentMain + (isVertical ? member->y() : member->x());
      const float rootExtent = isVertical ? member->height() : member->width();
      const float mainStart = std::max(0.0F, rootStart - mainInset);
      const float mainExtent = std::max(0.0F, std::min(shellMain, rootStart + rootExtent + mainInset) - mainStart);
      if (isVertical) {
        box->setPosition(crossInset, mainStart);
        box->setSize(crossExtent, mainExtent);
      } else {
        box->setPosition(mainStart, crossInset);
        box->setSize(mainExtent, crossExtent);
      }
      const float pillRadius = std::max(0.0F, std::min(box->width(), box->height()) * 0.5F);
      box->setRadius(std::min(pillRadius, std::max(0.0F, capsuleRadius - crossInset)));
    }
  }

  struct CapsuleMemberGeometry {
    std::size_t widgetIndex = 0;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    float sliceStart = 0.0F;
    float sliceEnd = 0.0F;

    [[nodiscard]] float mainStart(bool isVertical) const noexcept { return isVertical ? y : x; }
    [[nodiscard]] float mainEnd(bool isVertical) const noexcept {
      return mainStart(isVertical) + (isVertical ? height : width);
    }
  };

  [[nodiscard]] std::vector<CapsuleMemberGeometry>
  capsuleMemberSlices(const BarCapsuleRun& run, bool isVertical, float widgetHoverPadding) {
    std::vector<CapsuleMemberGeometry> members;
    if (run.shell == nullptr) {
      return members;
    }
    members.reserve(run.widgets.size());

    float shellX = 0.0F;
    float shellY = 0.0F;
    Node::absolutePosition(run.shell, shellX, shellY);
    for (std::size_t i = 0; i < run.widgets.size(); ++i) {
      Widget* widget = run.widgets[i];
      Node* root = widget != nullptr ? widget->outerNode() : nullptr;
      if (root == nullptr || !root->visible() || !root->participatesInLayout() || !root->hitTestVisible()) {
        continue;
      }

      float rootX = 0.0F;
      float rootY = 0.0F;
      Node::absolutePosition(root, rootX, rootY);
      members.push_back(
          CapsuleMemberGeometry{
              .widgetIndex = i,
              .x = rootX - shellX,
              .y = rootY - shellY,
              .width = root->width(),
              .height = root->height(),
          }
      );
    }

    const float shellMain = isVertical ? run.shell->height() : run.shell->width();
    for (std::size_t i = 0; i < members.size(); ++i) {
      auto& member = members[i];
      const float memberStart = member.mainStart(isVertical);
      const float memberEnd = member.mainEnd(isVertical);
      const float pad = widgetHoverPadding * run.widgets[member.widgetIndex]->contentScale();
      float before = pad;
      float after = pad;

      if (i > 0) {
        before = std::min(before, std::max(0.0F, (memberStart - members[i - 1].mainEnd(isVertical)) * 0.5F));
      }
      if (i + 1 < members.size()) {
        after = std::min(after, std::max(0.0F, (members[i + 1].mainStart(isVertical) - memberEnd) * 0.5F));
      }

      if (members.size() > 1) {
        if (i == 0) {
          before = after;
        }
        if (i + 1 == members.size()) {
          after = before;
        }
      }
      if (run.hasPaintedCapsuleBackground) {
        if (i == 0) {
          before = std::min(before, std::max(0.0F, memberStart));
        }
        if (i + 1 == members.size()) {
          after = std::min(after, std::max(0.0F, shellMain - memberEnd));
        }
      }
      member.sliceStart = memberStart - before;
      member.sliceEnd = memberEnd + after;
    }
    return members;
  }

  // Extends each member's hit target across the capsule padding and half the gap to its
  // neighbors, so hover/click coverage matches the capsule ink instead of stopping at the
  // content edge. Runs after applyBarWidgetHitTargets (which replaces outsets each layout).
  void extendCapsuleHitTargets(std::vector<BarCapsuleRun>& runs, bool isVertical, float widgetHoverPadding) {
    for (auto& run : runs) {
      if (run.shell == nullptr || run.bg == nullptr) {
        continue;
      }

      // Single runs (capsule or ghost pill): the hover overlay rect is the hit region.
      if (run.container == nullptr) {
        Widget* widget = !run.widgets.empty() ? run.widgets.front() : nullptr;
        Box* box = !run.hoverBoxes.empty() ? run.hoverBoxes.front() : run.bg;
        auto* area = widget != nullptr ? widget->outerNode() : nullptr;
        if (area == nullptr || box == nullptr || !area->visible() || !area->participatesInLayout()) {
          continue;
        }
        const float areaStart = isVertical ? area->y() : area->x();
        const float areaEnd = areaStart + (isVertical ? area->height() : area->width());
        const float boxStart = isVertical ? box->y() : box->x();
        const float boxEnd = boxStart + (isVertical ? box->height() : box->width());
        auto outset = area->hitTestOutset();
        if (isVertical) {
          outset.top += std::max(0.0F, areaStart - boxStart);
          outset.bottom += std::max(0.0F, boxEnd - areaEnd);
        } else {
          outset.left += std::max(0.0F, areaStart - boxStart);
          outset.right += std::max(0.0F, boxEnd - areaEnd);
        }
        area->setHitTestOutset(outset);
        continue;
      }

      // Tile only laid-out members; hidden ones keep stale geometry, and members clipped out of a
      // collapsed accordion are input-suppressed so their slice belongs to the visible member.
      const auto laidOut = capsuleMemberSlices(run, isVertical, widgetHoverPadding);
      const float shellMain = isVertical ? run.shell->height() : run.shell->width();
      const float paintedMain = isVertical ? run.bg->height() : run.bg->width();
      float minSliceStart = 0.0F;
      float maxSliceEnd = shellMain;
      for (std::size_t laidOutIndex = 0; laidOutIndex < laidOut.size(); ++laidOutIndex) {
        const auto& member = laidOut[laidOutIndex];
        const std::size_t i = member.widgetIndex;
        Node* area = run.widgets[i]->outerNode();
        const float memberStart = member.mainStart(isVertical);
        const float memberEnd = member.mainEnd(isVertical);
        const float sliceStart = member.sliceStart;
        // The last retained member owns the actual painted powerline tip,
        // which extends past the unmodified Flex shell by depth + run gap.
        const float sliceEnd = laidOutIndex + 1 == laidOut.size()
            ? std::max(member.sliceEnd, paintedMain)
            : member.sliceEnd;
        minSliceStart = std::min(minSliceStart, sliceStart);
        maxSliceEnd = std::max(maxSliceEnd, sliceEnd);

        auto outset = area->hitTestOutset();
        const float before = std::max(0.0F, memberStart - sliceStart);
        const float after = std::max(0.0F, sliceEnd - memberEnd);
        if (isVertical) {
          outset.top += before;
          outset.bottom += after;
        } else {
          outset.left += before;
          outset.right += after;
        }
        area->setHitTestOutset(outset);
      }
      auto shellOutset = run.shell->hitTestOutset();
      if (isVertical) {
        shellOutset.top += std::max(0.0F, -minSliceStart);
        shellOutset.bottom += std::max(0.0F, maxSliceEnd - shellMain);
      } else {
        shellOutset.left += std::max(0.0F, -minSliceStart);
        shellOutset.right += std::max(0.0F, maxSliceEnd - shellMain);
      }
      run.shell->setHitTestOutset(shellOutset);
    }
  }

  // Bind every retained member to the capsule background's final painted
  // polygon. Member hit outsets still partition a shared group, while the
  // common contour removes dead/overlapping diagonal ownership at joins.
  void syncCapsuleContourHitGeometry(std::vector<BarCapsuleRun>& runs, bool isVertical) {
    for (auto& run : runs) {
      if (run.bg == nullptr || run.shell == nullptr) continue;
      const auto contour = segmentContourFor(run.spec, isVertical);
      if (contour.kind == SegmentContourKind::None) continue;
      float bgX = 0.0F;
      float bgY = 0.0F;
      Node::absolutePosition(run.bg, bgX, bgY);
      for (Widget* widget : run.widgets) {
        auto* area = widget != nullptr ? dynamic_cast<InputArea*>(widget->root()) : nullptr;
        if (area == nullptr || area->hitShape() == InputArea::HitShape::Circle) continue;
        float areaX = 0.0F;
        float areaY = 0.0F;
        Node::absolutePosition(area, areaX, areaY);
        area->setSegmentHitContour(
            contour, bgX - areaX, bgY - areaY, run.bg->width(), run.bg->height()
        );
      }
    }
  }

  struct BarVisualGeometry {
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
  };

  struct BarSurfaceSpec {
    std::int32_t marginTop = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginBottom = 0;
    std::int32_t marginLeft = 0;
    std::uint32_t surfaceWidth = 0;
    std::uint32_t surfaceHeight = 0;
    std::int32_t exclusiveZone = 0;
  };

  [[nodiscard]] bool usesStableIslandMorphSurface(const BarConfig& config) {
    return config.sectionBackgrounds && config.islandMorph;
  }

  [[nodiscard]] std::optional<std::size_t> sourceSectionIndex(AttachedPanelSourceSection section) {
    switch (section) {
    case AttachedPanelSourceSection::Start: return 0;
    case AttachedPanelSourceSection::Center: return 1;
    case AttachedPanelSourceSection::End: return 2;
    case AttachedPanelSourceSection::Unknown: return std::nullopt;
    }
    return std::nullopt;
  }

  [[nodiscard]] BarSurfaceSpec
  computeBarSurfaceSpec(const BarConfig& barConfig, const ShellConfig::ShadowConfig& shadowConfig) {
    const bool vertical = (barConfig.position == "left" || barConfig.position == "right");
    const bool isBottom = barConfig.position == "bottom";
    const bool isRight = barConfig.position == "right";
    const std::int32_t mEnds = barConfig.marginEnds;
    const std::int32_t mEdge = barConfig.marginEdge;
    const auto sb = shell::surface_shadow::bleed(barConfig.shadow, shadowConfig, "bar", "bar");
    const int edgeGutter = barAutoHideEdgeGutter(barConfig);
    const auto concave = barConcaveShape(barConfig);
    const int insetL = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.left)));
    const int insetT = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.top)));
    const int insetR = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.right)));
    const int insetB = static_cast<int>(std::ceil(std::max(0.0F, concave.logicalInset.bottom)));
    // Reserve room for a concave-corner spike on the inner edge (opaque bar material),
    // in addition to the shadow bleed that renders beyond the spike tips.
    const int concaveBulge = static_cast<int>(std::lround(concave.innerBulge));

    const std::int32_t edgeMargin = barEdgeLayerMargin(barConfig, shadowConfig);

    BarSurfaceSpec spec;
    if (!vertical) {
      spec.marginLeft = usesStableIslandMorphSurface(barConfig) ? 0 : std::max(0, mEnds - sb.left - insetL);
      spec.marginRight = usesStableIslandMorphSurface(barConfig) ? 0 : std::max(0, mEnds - sb.right - insetR);
      if (isBottom) {
        if (edgeGutter > 0) {
          // Surface reaches the screen edge (no layer margin); the margin is folded
          // into the surface as a gutter on the edge side. Do not add the edge-side
          // bleed here — it lives inside the gutter, not beyond it.
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.up + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginBottom = edgeMargin;
          spec.surfaceHeight =
              static_cast<std::uint32_t>(sb.up + concaveBulge + barConfig.thickness + std::min(mEdge, sb.down));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceHeight = static_cast<std::uint32_t>(sb.down + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginTop = edgeMargin;
          spec.surfaceHeight =
              static_cast<std::uint32_t>(std::min(mEdge, sb.up) + barConfig.thickness + sb.down + concaveBulge);
        }
      }
    } else {
      spec.marginTop = usesStableIslandMorphSurface(barConfig) ? 0 : std::max(0, mEnds - sb.up - insetT);
      spec.marginBottom = usesStableIslandMorphSurface(barConfig) ? 0 : std::max(0, mEnds - sb.down - insetB);
      if (isRight) {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.left + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginRight = edgeMargin;
          spec.surfaceWidth =
              static_cast<std::uint32_t>(sb.left + concaveBulge + barConfig.thickness + std::min(mEdge, sb.right));
        }
      } else {
        if (edgeGutter > 0) {
          spec.surfaceWidth = static_cast<std::uint32_t>(sb.right + concaveBulge + barConfig.thickness + edgeGutter);
        } else {
          spec.marginLeft = edgeMargin;
          spec.surfaceWidth =
              static_cast<std::uint32_t>(std::min(mEdge, sb.left) + barConfig.thickness + sb.right + concaveBulge);
        }
      }
    }

    if (barConfig.sectionBackgrounds) {
      const auto extra = static_cast<std::uint32_t>(std::ceil(barConfig.islandHoverGrow + barConfig.islandHoverOffset));
      if (vertical) spec.surfaceWidth += extra; else spec.surfaceHeight += extra;
    }
    spec.exclusiveZone = barConfig.reserveSpace ? reservedBarExclusiveZone(barConfig, shadowConfig) : 0;
    return spec;
  }

  [[nodiscard]] float barInnerSurfaceExtension(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight
  ) {
    const auto base = computeBarSurfaceSpec(cfg, shadow);
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const float current = isVertical ? surfaceWidth : surfaceHeight;
    const auto normal = static_cast<float>(isVertical ? base.surfaceWidth : base.surfaceHeight);
    return std::max(0.0F, current - normal);
  }

  // Returns true when two bar configs would produce an identical layer-shell
  // surface (same anchor, size, exclusive zone, namespace). When true, an
  // existing BarInstance can be retained on reload and only its widget tree
  // rebuilt — avoiding the screen-shift caused by destroying and recreating
  // the exclusive zone.
  bool barConfigSurfaceFieldsEqual(
      const BarConfig& a, const BarConfig& b, const ShellConfig::ShadowConfig& previousShadow,
      const ShellConfig::ShadowConfig& nextShadow
  ) {
    const bool sameShadowSurface =
        (!a.shadow && !b.shadow) || shell::surface_shadow::sameSurfaceMetrics(previousShadow, nextShadow);
    return a.name == b.name
        && a.position == b.position
        && a.enabled == b.enabled
        && a.autoHide == b.autoHide
        && a.smartAutoHide == b.smartAutoHide
        && a.reserveSpace == b.reserveSpace
        && a.layer == b.layer
        && a.thickness == b.thickness
        // Corner radii feed the concave-corner bulge, which changes the surface size.
        && a.radiusTopLeft == b.radiusTopLeft
        && a.radiusTopRight == b.radiusTopRight
        && a.radiusBottomLeft == b.radiusBottomLeft
        && a.radiusBottomRight == b.radiusBottomRight
        && a.concaveEdgeCorners == b.concaveEdgeCorners
        && a.sectionBackgrounds == b.sectionBackgrounds
        && a.islandHoverGrow == b.islandHoverGrow
        && a.islandHoverOffset == b.islandHoverOffset
        && a.islandMorph == b.islandMorph
        && a.maxLength == b.maxLength
        && a.marginEnds == b.marginEnds
        && a.marginEdge == b.marginEdge
        && a.shadow == b.shadow
        && sameShadowSurface
        // Section identity/order determines retained scene nodes and panel source
        // anchors. Recreate the surface before those nodes can be removed.
        && a.sections == b.sections
        && a.monitorOverrides == b.monitorOverrides;
  }

  bool barSurfaceOrderRequiresRecreate(const std::vector<BarConfig>& previous, const std::vector<BarConfig>& next) {
    std::vector<std::string> preserved;
    preserved.reserve(previous.size());
    for (const auto& oldBar : previous) {
      const auto it = std::ranges::find(next, oldBar.name, &BarConfig::name);
      if (it != next.end()) {
        preserved.push_back(oldBar.name);
      }
    }

    if (preserved.size() > next.size()) {
      return true;
    }
    for (std::size_t i = 0; i < preserved.size(); ++i) {
      if (next[i].name != preserved[i]) {
        return true;
      }
    }
    return false;
  }

  BarVisualGeometry computeBarVisualGeometry(
      const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, float surfaceWidth, float surfaceHeight,
      float innerSurfaceExtension = 0.0F
  ) {
    const auto barThickness = static_cast<float>(cfg.thickness);
    const auto marginEnds = static_cast<float>(cfg.marginEnds);
    const auto marginEdge = static_cast<float>(cfg.marginEdge);
    const bool isBottom = cfg.position == "bottom";
    const bool isRight = cfg.position == "right";
    const bool isVertical = (cfg.position == "left" || cfg.position == "right");
    const auto sbi = shell::surface_shadow::bleed(cfg.shadow, shadow, "bar", "bar");
    const auto concave = barConcaveShape(cfg);
    const float insetL = std::ceil(std::max(0.0F, concave.logicalInset.left));
    const float insetT = std::ceil(std::max(0.0F, concave.logicalInset.top));
    const float insetR = std::ceil(std::max(0.0F, concave.logicalInset.right));
    const float insetB = std::ceil(std::max(0.0F, concave.logicalInset.bottom));
    const auto bleedLeft = static_cast<float>(sbi.left);
    const auto bleedRight = static_cast<float>(sbi.right);
    const auto bleedUp = static_cast<float>(sbi.up);
    const auto bleedDown = static_cast<float>(sbi.down);
    const auto stableMainInsets = bar_island_morph::stableSurfaceMainInsets(
        cfg.position, marginEnds,
        {.left = bleedLeft, .top = bleedUp, .right = bleedRight, .bottom = bleedDown},
        {.left = insetL, .top = insetT, .right = insetR, .bottom = insetB}
    );
    // For bottom/right bars the inner edge is the origin side, so the concave spike
    // pushes the body inward by its bulge. Top/left bars grow away from the origin
    // and need no body shift. Gutter (auto-hide) placements derive from the surface
    // size, which already includes the bulge, so they shift automatically.
    const float concaveBulge = concave.innerBulge;

    if (isVertical) {
      // Vertical bar: edge gap is left/right, ends inset is top/bottom.
      const float y = usesStableIslandMorphSurface(cfg)
          ? stableMainInsets.start
          : std::min(marginEnds, bleedUp) + insetT;
      float x = isRight ? (bleedLeft + concaveBulge) : std::min(marginEdge, bleedLeft);
      if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
        // The gutter equals marginEdge and sits between the screen edge and the bar.
        // Position the bar exactly marginEdge from the edge so it matches the
        // non-auto-hide placement; the edge-side shadow bleeds into the gutter.
        if (isRight) {
          x = surfaceWidth - static_cast<float>(gutter) - barThickness;
        } else {
          x = static_cast<float>(gutter);
        }
      } else if (isRight) {
        x += innerSurfaceExtension + (cfg.sectionBackgrounds ? std::ceil(cfg.islandHoverGrow + cfg.islandHoverOffset) : 0.F);
      }
      return {
          .x = x,
          .y = y,
          .width = barThickness,
          .height = surfaceHeight - y - (usesStableIslandMorphSurface(cfg)
                  ? stableMainInsets.end
                  : std::min(marginEnds, bleedDown) + insetB),
      };
    }

    // Horizontal bar: edge gap is top/bottom, ends inset is left/right.
    const float x = usesStableIslandMorphSurface(cfg)
        ? stableMainInsets.start
        : std::min(marginEnds, bleedLeft) + insetL;
    float y = isBottom ? (bleedUp + concaveBulge) : std::min(marginEdge, bleedUp);
    if (const int gutter = barAutoHideEdgeGutter(cfg); gutter > 0) {
      if (isBottom) {
        y = surfaceHeight - static_cast<float>(gutter) - barThickness;
      } else {
        y = static_cast<float>(gutter);
      }
    } else if (isBottom) {
      y += innerSurfaceExtension + (cfg.sectionBackgrounds ? std::ceil(cfg.islandHoverGrow + cfg.islandHoverOffset) : 0.F);
    }
    return {
        .x = x,
        .y = y,
        .width = surfaceWidth - x - (usesStableIslandMorphSurface(cfg)
                ? stableMainInsets.end
                : std::min(marginEnds, bleedRight) + insetR),
        .height = barThickness,
    };
  }

  [[nodiscard]] InputRect
  barContentInputRegion(const BarConfig& cfg, const ShellConfig::ShadowConfig& shadow, int surfW, int surfH) {
    const float innerSurfaceExtension =
        barInnerSurfaceExtension(cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH));
    const auto barVisual = computeBarVisualGeometry(
        cfg, shadow, static_cast<float>(surfW), static_cast<float>(surfH), innerSurfaceExtension
    );
    return InputRect{
        static_cast<int>(barVisual.x), static_cast<int>(barVisual.y), static_cast<int>(barVisual.width),
        static_cast<int>(barVisual.height)
    };
  }

  std::pair<float, float> computeAutoHideHiddenDelta(
      bool isVertical, bool isBottom, bool isRight, float w, float h, float contentLeft, float contentTop,
      float contentRight, float contentBottom
  ) {
    const float k = kAutoHideSlideExtraPx;
    if (!isVertical) {
      if (isBottom) {
        return {0.0F, (h - contentTop) + k};
      }
      return {0.0F, -(contentBottom + k)};
    }
    if (isRight) {
      return {(w - contentLeft) + k, 0.0F};
    }
    return {-(contentRight + k), 0.0F};
  }

  void layoutBarSectionBackgrounds(BarInstance& instance) {
    if (instance.bg == nullptr || instance.contentClip == nullptr) return;
    const bool enabled = instance.barConfig.sectionBackgrounds;
    instance.bg->setVisible(!enabled);
    if (instance.shadow != nullptr) instance.shadow->setVisible(!enabled);
    const std::array<Flex*, 3> sections{instance.startSection, instance.centerSection, instance.endSection};
    const std::array<Node*, 3> slots{instance.startSlot, instance.centerSlot, instance.endSlot};
    const bool vertical = instance.barConfig.position == "left" || instance.barConfig.position == "right";
    const float span = vertical ? instance.contentClip->height() : instance.contentClip->width();
    const float cross = vertical ? instance.contentClip->width() : instance.contentClip->height();
    std::array<bar_sections::Extent, 3> extents{};
    for (std::size_t i = 0; i < sections.size(); ++i) {
      const auto* section = sections[i];
      const auto* slot = slots[i];
      if (section == nullptr || slot == nullptr || !enabled) continue;
      const bool visibleContent = std::ranges::any_of(section->children(), [](const auto& child) {
        return child->visible() && child->participatesInLayout() && child->width() > 0 && child->height() > 0;
      });
      const float slotStart = vertical ? slot->y() : slot->x();
      const float slotEnd = slotStart + (vertical ? slot->height() : slot->width());
      const float start = slotStart + (vertical ? section->y() : section->x());
      const float end = start + (vertical ? section->height() : section->width());
      extents[i] = bar_sections::clippedExtent(slotStart, slotEnd, start, end, visibleContent);
    }
    auto padded = bar_sections::paddedExtents(
        extents, span, static_cast<float>(instance.barConfig.padding),
        static_cast<float>(instance.barConfig.widgetSpacing));
    if (instance.islandMorphExtents.has_value()) {
      for (std::size_t index = 0; index < padded.size(); ++index) {
        const auto& extent = (*instance.islandMorphExtents)[index];
        padded[index] = {extent.start, extent.end, extent.visible};
      }
    }
    std::optional<std::size_t> panelOwnedSection;
    if (instance.attachedPanelGeometry.has_value() && instance.attachedPanelGeometry->panelOwnsSource) {
      panelOwnedSection = sourceSectionIndex(instance.attachedPanelGeometry->source.section);
    }
    auto style = instance.bg->style();
    // Each detached lane is a rounded surface; concave edge junctions belong to continuous bars.
    style.corners = {};
    style.logicalInset = {};
    for (std::size_t i = 0; i < sections.size(); ++i) {
      auto* background = instance.sectionBackgrounds[i];
      auto* shadow = instance.sectionShadows[i];
      if (background == nullptr || shadow == nullptr) continue;
      auto rect = bar_sections::rectangle(padded[i], cross, vertical);
      const auto& hover = instance.hoveredPanelSources[i];
      if (hover.valid() && !instance.attachedPanelGeometry.has_value())
        rect = {hover.x, hover.y, hover.width, hover.height};
      const bool visible = enabled && padded[i].visible && rect.width > 0 && rect.height > 0
          && panelOwnedSection != i;
      background->setVisible(visible);
      shadow->setVisible(visible && instance.shadow != nullptr);
      if (!visible) continue;
      const float x = instance.contentClip->x() + rect.x;
      const float y = instance.contentClip->y() + rect.y;
      background->setStyle(style);
      background->setPosition(x, y);
      background->setSize(rect.width, rect.height);
      if (instance.shadow != nullptr) {
        auto shadowStyle = instance.shadow->style();
        shadowStyle.corners = {};
        shadowStyle.logicalInset = {};
        const float dx = x - instance.bg->x();
        const float dy = y - instance.bg->y();
        shadowStyle.shadowExclusionOffsetX += dx;
        shadowStyle.shadowExclusionOffsetY += dy;
        shadow->setStyle(shadowStyle);
        shadow->setPosition(instance.shadow->x() + dx, instance.shadow->y() + dy);
        shadow->setSize(rect.width, rect.height);
      }
    }
  }

  void appendBackgroundRegion(std::vector<InputRect>& regions, const Box* background) {
      if (background == nullptr || !background->visible()) return;
      float x = 0, y = 0;
      Node::absolutePosition(background, x, y);
      const auto& style = background->style();
      const int px = static_cast<int>(std::lround(x));
      const int py = static_cast<int>(std::lround(y));
      const int pw = static_cast<int>(std::lround(background->width()));
      const int ph = static_cast<int>(std::lround(background->height()));
      auto strips = style.segmentContour.kind != SegmentContourKind::None
          ? Surface::tessellateSegmentContour(px, py, pw, ph, style.segmentContour)
          : Surface::tessellateShape(
                px, py, pw, ph, style.corners, style.logicalInset, style.radius, 1,
                style.cornerPower.value_or(background->cornerPower()));
      regions.insert(regions.end(), strips.begin(), strips.end());
  }

  void appendCapsuleRegions(std::vector<InputRect>& regions, const std::vector<BarCapsuleRun>& runs) {
    for (const auto& run : runs) {
      if (run.shell == nullptr || !run.shell->visible() || !run.shell->participatesInLayout()) continue;
      appendBackgroundRegion(regions, run.bg);
    }
  }

  [[nodiscard]] std::vector<InputRect> barSectionRegions(const BarInstance& instance, bool stableInputEnvelope = false) {
    std::vector<InputRect> regions;
    for (const auto& section : instance.dynamicSections) {
      appendBackgroundRegion(regions, section.background);
      if (stableInputEnvelope) appendBackgroundRegion(regions, section.inputEnvelope);
      appendCapsuleRegions(regions, section.capsuleRuns);
    }
    for (const auto* background : instance.sectionBackgrounds) {
      appendBackgroundRegion(regions, background);
    }
    // Capsule tips can extend beyond a section's rounded body. Include their
    // analytic strips in the Wayland input/blur ownership union.
    appendCapsuleRegions(regions, instance.startCapsuleRuns);
    appendCapsuleRegions(regions, instance.centerCapsuleRuns);
    appendCapsuleRegions(regions, instance.endCapsuleRuns);
    return regions;
  }

  void applyBarShadowStyle(
      BarInstance& instance, const ShellConfig::ShadowConfig& shadowConfig, float surfaceWidth, float surfaceHeight
  ) {
    if (instance.shadow == nullptr) {
      return;
    }

    const auto concave = barConcaveShape(instance.barConfig);
    const float innerSurfaceExtension =
        barInnerSurfaceExtension(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight);
    const auto barVisual =
        computeBarVisualGeometry(instance.barConfig, shadowConfig, surfaceWidth, surfaceHeight, innerSurfaceExtension);
    // Shadow follows the same shape as the background: the body expanded outward by
    // the concave inset into the visual rect, so concave spikes cast a matching shadow.
    const float barAreaW = barVisual.width + concave.logicalInset.left + concave.logicalInset.right;
    const float barAreaH = barVisual.height + concave.logicalInset.top + concave.logicalInset.bottom;
    const float bgOpacity = std::clamp(instance.barConfig.backgroundOpacity, 0.0F, 1.0F);
    const auto shadowOff = shadowDirectionOffset(shadowConfig.direction);
    const float shadowX = barVisual.x - concave.logicalInset.left + static_cast<float>(shadowOff.x);
    const float shadowY = barVisual.y - concave.logicalInset.top + static_cast<float>(shadowOff.y);
    RoundedRectStyle shadowStyle = shell::surface_shadow::style(
        shadowConfig, bgOpacity,
        shell::surface_shadow::Shape{
            .corners = concave.corners, .logicalInset = concave.logicalInset, .radius = concave.radii
        }
    );

    const bool panelShadowExclusion = instance.attachedPanelGeometry.has_value()
        && instance.attachedPanelGeometry->width > 0.0F
        && instance.attachedPanelGeometry->height > 0.0F;
    if (panelShadowExclusion) {
      const auto& attached = *instance.attachedPanelGeometry;
      const float convexRadius = std::max(0.0F, attached.cornerRadius);
      const float bulgeRadius = std::max(0.0F, attached.bulgeRadius);
      const std::string_view barPosition = instance.barConfig.position;
      const auto corners = attached_panel::cornerShapes(barPosition);
      const auto pickRadius = [&](CornerShape shape) {
        return shape == CornerShape::Concave ? bulgeRadius : convexRadius;
      };
      shadowStyle.shadowExclusion = true;
      shadowStyle.shadowExclusionPower = attached.cornerPower;
      shadowStyle.shadowExclusionOffsetX = shadowX - attached.x;
      shadowStyle.shadowExclusionOffsetY = shadowY - attached.y;
      shadowStyle.shadowExclusionWidth = attached.width;
      shadowStyle.shadowExclusionHeight = attached.height;
      shadowStyle.shadowExclusionCorners = corners;
      shadowStyle.shadowExclusionLogicalInset = attached_panel::logicalInset(barPosition, bulgeRadius);
      shadowStyle.shadowExclusionRadius =
          Radii{pickRadius(corners.tl), pickRadius(corners.tr), pickRadius(corners.br), pickRadius(corners.bl)};
    }

    auto configureShadow = [&](Box* node, float x, float y) {
      if (node == nullptr) {
        return;
      }
      node->setStyle(shadowStyle);
      node->setZIndex(-1);
      node->setPosition(x, y);
      node->setSize(barAreaW, barAreaH);
    };

    instance.shadow->setHitTestVisible(false);
    instance.shadow->setVisible(true);
    configureShadow(instance.shadow, shadowX, shadowY);

    if (instance.shadowLeftClip != nullptr) {
      instance.shadowLeftClip->setVisible(false);
    }
    if (instance.shadowRightClip != nullptr) {
      instance.shadowRightClip->setVisible(false);
    }
    layoutBarSectionBackgrounds(instance);
  }

  void layoutBarSections(
      BarInstance& instance, Renderer& renderer, float barAreaW, float barAreaH, float padding, bool isVertical,
      float mainSpan, float mainInsetStart, float mainInsetEnd
  ) {
    const float slotCross = isVertical ? barAreaW : barAreaH;
    const std::array<Flex*, 3> sectionNodes{instance.startSection, instance.centerSection, instance.endSection};
    const std::array<Node*, 3> slotNodes{instance.startSlot, instance.centerSlot, instance.endSlot};
    instance.islandMorphExtents.reset();
    instance.hoveredPanelSources = {};
    for (auto* section : sectionNodes) {
      if (section == nullptr) continue;
      section->setVisible(true);
      section->setOpacity(1.0F);
      section->setHitTestVisible(true);
    }

    // Capsule cross-size is a fraction of the bar thickness (capsule_thickness), the same for every capsule
    // regardless of per-widget content scale. The max() guard keeps a thin bar from yielding a 0px capsule.
    const float capsuleCross = std::max(1.0F, std::round(slotCross * instance.barConfig.capsuleThickness));

    auto layoutWidgets = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (auto* tray = dynamic_cast<TrayWidget*>(widget.get())) {
          tray->setCapsuleCross(capsuleCross);
        }
        if (widget->root() != nullptr) {
          widget->layout(renderer, barAreaW, barAreaH);
        }
      }
    };
    layoutWidgets(instance.startWidgets);
    layoutWidgets(instance.centerWidgets);
    layoutWidgets(instance.endWidgets);

    const float capsuleRunSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
    auto finalizeCapsules = [isVertical, capsuleCross, capsuleRunSpacing,
                             widgetHoverPadding = instance.barConfig.widgetCapsulePadding,
                             &renderer](std::vector<BarCapsuleRun>& runs) {
      for (auto& run : runs) {
        Node* shell = run.shell;
        Box* bg = run.bg;
        Node* content = run.content;
        if (shell == nullptr || bg == nullptr || content == nullptr) {
          continue;
        }
        if (run.container != nullptr) {
          run.container->layout(renderer);
        }

        bool hasVisibleContent = false;
        bool hasCapsuleContent = false;
        for (Widget* widget : run.widgets) {
          if (widget == nullptr || widget->root() == nullptr) {
            continue;
          }
          hasVisibleContent = hasVisibleContent || widget->root()->visible();
          hasCapsuleContent = hasCapsuleContent || widget->shouldShowBarCapsule();
        }
        const bool hasPaintedFill = resolveColorSpec(scaleAlpha(run.spec.fill, run.spec.opacity)).a > 0.0F;
        const bool hasPaintedBorder = run.spec.border.has_value() && resolveColorSpec(*run.spec.border).a > 0.0F;
        run.hasPaintedCapsuleBackground = hasCapsuleContent && (hasPaintedFill || hasPaintedBorder);

        shell->setVisible(hasVisibleContent);
        const float scale = run.contentScale;
        const float iw = content->width();
        const float ih = content->height();
        auto memberMainPos = [isVertical](const Node* node) { return isVertical ? node->y() : node->x(); };
        auto memberMainExtent = [isVertical](const Node* node) { return isVertical ? node->height() : node->width(); };
        auto clearAccordionSuppression = [&run]() {
          for (Widget* widget : run.widgets) {
            if (widget != nullptr) {
              widget->setBarPointerSuppressed(false);
            }
          }
        };
        if (!hasCapsuleContent) {
          shell->setSize(iw, ih);
          if (run.accordionClip != nullptr) {
            run.accordionClip->setPosition(0.0F, 0.0F);
            run.accordionClip->setSize(iw, ih);
          }
          content->setPosition(0.0F, 0.0F);
          bg->setVisible(false);
          bg->setPosition(0.0F, 0.0F);
          bg->setSize(iw, ih);
          placeCapsuleHoverBoxes(run, isVertical, iw, ih, 0.0F, 0.0F, std::min(iw, ih) * 0.5F, widgetHoverPadding);
          // No pill to hover: the group renders full-size, so nothing may stay input-suppressed.
          if (run.accordion) {
            clearAccordionSuppression();
          }
          continue;
        }
        const float pad = run.spec.padding * scale;
        const float padMain = pad;
        const float fullMain = isVertical ? ih : iw;
        // Collapsed accordion: the shell only spans its always-visible member; the rest are clipped
        // out and revealed as the hover progress lerps the main extent up to the full content size.
        const bool accordionStartDir = run.accordion && run.accordionDirection == BarAccordionDirection::Start;
        float revealMain = fullMain;
        if (run.accordion) {
          const Node* visibleMember = nullptr;
          auto laidOut = [](const Widget* widget) {
            const Node* node = widget != nullptr ? widget->outerNode() : nullptr;
            return node != nullptr && node->visible() && node->participatesInLayout() ? node : nullptr;
          };
          if (run.accordionVisibleIndex < run.widgets.size()) {
            visibleMember = laidOut(run.widgets[run.accordionVisibleIndex]);
          }
          if (visibleMember == nullptr) {
            // The configured first member is hidden: anchor on the laid-out member at the pill's fixed edge.
            for (Widget* widget : run.widgets) {
              if (const Node* node = laidOut(widget); node != nullptr) {
                visibleMember = node;
                if (!accordionStartDir) {
                  break;
                }
              }
            }
          }
          if (visibleMember != nullptr) {
            const float visMain = memberMainExtent(visibleMember);
            const float progress = std::clamp(run.accordionProgress, 0.0F, 1.0F);
            revealMain = visMain + (fullMain - visMain) * progress;
          }
        }
        // Cross-size is the fixed capsuleCross, independent of per-widget content scale: scaling a widget
        // enlarges its glyph/text inside the fixed-height pill rather than resizing the capsule (so a
        // differently scaled member can't grow or split its capsule group). The main axis is content plus
        // per-widget padding, so an icon-only widget reads as a near-circular pill at the default padding
        // and widens as padding increases.
        const float shellMain = revealMain + 2.0F * padMain;
        const float shellCross = capsuleCross;
        const float shellW = isVertical ? shellCross : shellMain;
        const float shellH = isVertical ? shellMain : shellCross;
        // Start-direction accordions pin the content's end edge, so the always-visible last member
        // stays put while the hidden ones unfold off the pill's leading edge.
        const float contentMain = accordionStartDir ? shellMain - padMain - fullMain : padMain;
        const float contentX = isVertical ? (shellW - iw) * 0.5F : contentMain;
        const float contentY = isVertical ? contentMain : (shellH - ih) * 0.5F;
        shell->setSize(shellW, shellH);
        bg->setVisible(true);
        bg->setPosition(0.0F, 0.0F);
        bg->setSize(shellW, shellH);
        if (run.accordionClip != nullptr) {
          // Reveal window: [padMain, padMain + revealMain] in shell coordinates, for both directions.
          if (isVertical) {
            run.accordionClip->setPosition(0.0F, padMain);
            run.accordionClip->setSize(shellCross, revealMain);
          } else {
            run.accordionClip->setPosition(padMain, 0.0F);
            run.accordionClip->setSize(revealMain, shellCross);
          }
          content->setPosition(contentX - (isVertical ? 0.0F : padMain), contentY - (isVertical ? padMain : 0.0F));
        } else {
          content->setPosition(contentX, contentY);
        }
        const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
        const float capsuleRadius = radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(shellW, shellH)
                                                            : std::max(0.0F, std::min(shellW, shellH) * 0.5F);
        bg->setRadius(capsuleRadius);
        const auto contour = segmentContourFor(run.spec, isVertical);
        bg->setSegmentContour(contour);
        const float depth = segment_contour::clampedDepth(shellMain, shellCross, contour.depth);
        const float trailingExtension = segment_contour::hasTrailingCut(contour.kind)
            ? depth + std::max(0.0F, run.spec.widgetSpacing.value_or(capsuleRunSpacing))
            : 0.0F;
        if (contour.kind != SegmentContourKind::None) {
          shell->setClipChildren(false);
          bg->setSize(
              isVertical ? shellW : shellW + trailingExtension,
              isVertical ? shellH + trailingExtension : shellH
          );
        } else {
          shell->setClipChildren(true);
        }
        for (Box* hover : run.hoverBoxes) {
          if (hover != nullptr) hover->setSegmentContour(contour);
        }
        if (run.container == nullptr) {
          placeCapsuleHoverBoxes(
              run, isVertical, isVertical ? shellW : shellW + trailingExtension,
              isVertical ? shellH + trailingExtension : shellH, contentX, contentY, capsuleRadius,
              widgetHoverPadding
          );
        }
        // Members outside the reveal window are clipped out; while collapsed (or collapsing) the
        // non-primary members are pointer-suppressed by reveal window so their hidden slots don't
        // capture clicks. While expanded, every valid layout member stays unsuppressed so hover
        // tracking and member interactions remain active across the reveal animation.
        if (run.accordion) {
          for (Widget* widget : run.widgets) {
            const Node* node = widget != nullptr ? widget->outerNode() : nullptr;
            if (widget == nullptr || node == nullptr || !node->visible() || !node->participatesInLayout()) {
              if (widget != nullptr) {
                widget->setBarPointerSuppressed(false);
              }
              continue;
            }
            if (run.accordionExpanded) {
              widget->setBarPointerSuppressed(false);
            } else {
              const float memberStart = contentMain + memberMainPos(node);
              const float memberEnd = memberStart + memberMainExtent(node);
              widget->setBarPointerSuppressed(
                  !(memberStart >= padMain - 0.5F && memberEnd <= padMain + revealMain + 0.5F)
              );
            }
          }
        }
      }
    };
    finalizeCapsules(instance.startCapsuleRuns);
    finalizeCapsules(instance.centerCapsuleRuns);
    finalizeCapsules(instance.endCapsuleRuns);

    // When bar touches screen edge, put the padding inside the sections, and extend the hit targets of
    // the first/last widgets to cover the area. So clicking on the screen edge still triggers the widget.
    const bool screenEdgeClick = !instance.barConfig.sectionBackgrounds && instance.barConfig.marginEnds == 0 && padding > 0;
    const float paddingInsideSection = screenEdgeClick ? padding : 0.0F;
    const float contentMainStart = mainInsetStart + (screenEdgeClick ? 0.0F : padding);
    const float contentMainEnd =
        std::max(contentMainStart, mainSpan - mainInsetEnd - (screenEdgeClick ? 0.0F : padding));
    const float contentMainSpan = std::max(0.0F, contentMainEnd - contentMainStart);

    if (!instance.dynamicSections.empty()) {
      std::vector<noctalia::bar::dynamic_sections::Request> requests;
      requests.reserve(instance.dynamicSections.size());
      std::vector<noctalia::bar::dynamic_sections::LayoutRole> layoutRoles;
      layoutRoles.reserve(instance.dynamicSections.size());
      std::vector<float> hoverProgress;
      hoverProgress.reserve(instance.dynamicSections.size());
      for (auto& section : instance.dynamicSections) {
        layoutWidgets(section.widgets);
        finalizeCapsules(section.capsuleRuns);
        section.content->setJustify(section.config.alignment == BarCenterAlignment::Start
                ? FlexJustify::Start
                : section.config.alignment == BarCenterAlignment::End ? FlexJustify::End : FlexJustify::Center);
        section.content->layout(renderer);
        float progress = 0.0F;
        for (const auto& widget : section.widgets)
          if (widget != nullptr) progress = std::max(progress, widget->barHoverProgress());
        hoverProgress.push_back(progress);
        const float natural = isVertical ? section.content->height() : section.content->width();
        const float painted = natural + (instance.barConfig.sectionBackgrounds ? 2.0F * std::max(0.0F, padding) : 0.0F);
        const float hoverMain = instance.barConfig.sectionBackgrounds
            ? 2.0F * std::max(0.0F, instance.barConfig.islandHoverGrow) * progress
            : 0.0F;
        const float anchor = section.config.anchor == BarCenterAlignment::Start
            ? contentMainStart
            : section.config.anchor == BarCenterAlignment::End ? contentMainEnd : contentMainStart + contentMainSpan * 0.5F;
        const float alignment = section.config.alignment == BarCenterAlignment::Start
            ? 0.0F : section.config.alignment == BarCenterAlignment::End ? 1.0F : 0.5F;
        requests.push_back({anchor, painted + hoverMain, alignment, section.config.offset, section.config.allowOverlap});
        using DynamicRole = noctalia::bar::dynamic_sections::LayoutRole;
        layoutRoles.push_back(section.config.layoutRole == BarSectionLayoutRole::Start ? DynamicRole::Start
            : section.config.layoutRole == BarSectionLayoutRole::Center ? DynamicRole::Center
            : section.config.layoutRole == BarSectionLayoutRole::End ? DynamicRole::End : DynamicRole::Free);
      }
      // Keep the collapsed layout independent of the panel's animated extent.
      // It is the destination of the closing animation, never its current frame.
      auto compactRequests = requests;
      for (std::size_t i = 0; i < compactRequests.size(); ++i)
        compactRequests[i].size -= instance.barConfig.sectionBackgrounds
            ? 2.0F * std::max(0.0F, instance.barConfig.islandHoverGrow) * hoverProgress[i] : 0.0F;
      if (usesStableIslandMorphSurface(instance.barConfig) && instance.attachedPanelGeometry.has_value()
          && !instance.attachedPanelGeometry->source.sectionId.empty()) {
        const auto& attached = *instance.attachedPanelGeometry;
        const auto it = std::ranges::find(instance.dynamicSections, attached.source.sectionId,
                                          [](const DynamicBarSection& section) { return section.config.id; });
        if (it != instance.dynamicSections.end()) {
          const auto index = static_cast<std::size_t>(std::distance(instance.dynamicSections.begin(), it));
          auto& request = requests[index];
          const float requestedStart = request.anchor - request.size * request.alignment + request.offset;
          const float clipMain = isVertical ? instance.contentClip->y() : instance.contentClip->x();
          const float targetStart = (isVertical ? attached.finalOutputY : attached.finalOutputX) - clipMain;
          const float targetSize = isVertical ? attached.finalOutputHeight : attached.finalOutputWidth;
          const float progress = std::clamp(attached.revealProgress, 0.0F, 1.0F);
          request.anchor = requestedStart + (targetStart - requestedStart) * progress;
          request.size += (std::max(0.0F, targetSize) - request.size) * progress;
          request.alignment = 0.0F;
          request.offset = 0.0F;
        }
      }
      const auto edgePolicy = instance.barConfig.centeredSections
              && instance.barConfig.edgeClusterPolicy == BarEdgeClusterPolicy::Edge
          ? BarEdgeClusterPolicy::FollowCenter : instance.barConfig.edgeClusterPolicy;
      using DynamicPolicy = noctalia::bar::dynamic_sections::EdgePolicy;
      noctalia::bar::dynamic_sections::applyLanePolicy(
          requests, layoutRoles, contentMainStart, contentMainEnd,
          static_cast<float>(instance.barConfig.widgetSpacing),
          instance.barConfig.centerAlignment == BarCenterAlignment::Start ? 0.0F
              : instance.barConfig.centerAlignment == BarCenterAlignment::End ? 1.0F : 0.5F,
          edgePolicy == BarEdgeClusterPolicy::FollowCenter ? DynamicPolicy::FollowCenter
              : edgePolicy == BarEdgeClusterPolicy::Equidistant ? DynamicPolicy::Equidistant : DynamicPolicy::Edge);
      noctalia::bar::dynamic_sections::applyLanePolicy(
          compactRequests, layoutRoles, contentMainStart, contentMainEnd,
          static_cast<float>(instance.barConfig.widgetSpacing),
          instance.barConfig.centerAlignment == BarCenterAlignment::Start ? 0.0F
              : instance.barConfig.centerAlignment == BarCenterAlignment::End ? 1.0F : 0.5F,
          edgePolicy == BarEdgeClusterPolicy::FollowCenter ? DynamicPolicy::FollowCenter
              : edgePolicy == BarEdgeClusterPolicy::Equidistant ? DynamicPolicy::Equidistant : DynamicPolicy::Edge);
      const auto compactExtents = noctalia::bar::dynamic_sections::resolve(
          compactRequests, contentMainStart, contentMainEnd, static_cast<float>(instance.barConfig.widgetSpacing));
      const auto extents = noctalia::bar::dynamic_sections::resolve(
          requests, contentMainStart, contentMainEnd, static_cast<float>(instance.barConfig.widgetSpacing));
      const bool reverseCross = instance.barConfig.position == "bottom" || instance.barConfig.position == "right";
      for (std::size_t i = 0; i < instance.dynamicSections.size(); ++i) {
        auto& section = instance.dynamicSections[i];
        const bool panelOwnsSection = instance.attachedPanelGeometry.has_value()
            && instance.attachedPanelGeometry->panelOwnsSource
            && instance.attachedPanelGeometry->source.sectionId == section.config.id;
        const bool paintsOwnBackground = instance.barConfig.sectionBackgrounds
            || section.config.background.has_value() || section.config.backgroundOpacity.has_value()
            || section.config.border.has_value() || section.config.borderWidth.has_value()
            || section.config.materialMode != BarMaterialMode::Inherit
            || section.config.shader != BarSectionShader::Inherit;
        const float length = std::max(0.0F, extents[i].end - extents[i].start);
        const float progress = hoverProgress[i];
        const float crossGrow = instance.barConfig.sectionBackgrounds
            ? std::max(0.0F, instance.barConfig.islandHoverGrow) * progress : 0.0F;
        const float hoverOffset = instance.barConfig.sectionBackgrounds
            ? instance.barConfig.islandHoverOffset * progress : 0.0F;
        const float crossStart = section.config.crossOffset
            + (reverseCross ? -hoverOffset - crossGrow : hoverOffset);
        const float crossLength = slotCross + crossGrow;
        if (isVertical) {
          section.slot->setPosition(crossStart, extents[i].start);
          section.slot->setSize(crossLength, length);
          section.content->setPosition((crossLength - section.content->width()) * 0.5F,
                                       (length - section.content->height()) * 0.5F);
        } else {
          section.slot->setPosition(extents[i].start, crossStart);
          section.slot->setSize(length, crossLength);
          section.content->setPosition((length - section.content->width()) * 0.5F,
                                       (crossLength - section.content->height()) * 0.5F);
        }
        section.slot->setClipChildren(!section.config.allowOverlap);
        section.content->setOpacity(panelOwnsSection ? 0.0F : 1.0F);
        applyBarWidgetHitTargets(
            section.content, section.slot, isVertical,
            std::max(0.0F, instance.barConfig.islandHoverGrow + std::abs(instance.barConfig.islandHoverOffset)));

        const float absoluteX = instance.contentClip->x() + (isVertical ? crossStart : extents[i].start);
        const float absoluteY = instance.contentClip->y() + (isVertical ? extents[i].start : crossStart);
        const float width = isVertical ? crossLength : length;
        const float height = isVertical ? length : crossLength;
        section.background->setPosition(absoluteX, absoluteY);
        section.background->setSize(width, height);
        section.background->setVisible(!panelOwnsSection && paintsOwnBackground && length > 0.0F);
        section.background->setRadius(std::max(0.0F, static_cast<float>(instance.barConfig.radius)));
        float opacity = section.config.backgroundOpacity.value_or(instance.barConfig.backgroundOpacity);
        const auto materialMode = section.config.materialMode == BarMaterialMode::Inherit
            ? instance.barConfig.materialMode : section.config.materialMode;
        if (materialMode == BarMaterialMode::Solid) opacity = 1.0F;
        if (materialMode == BarMaterialMode::Glass) opacity = std::min(opacity, 0.72F);
        if (materialMode == BarMaterialMode::Transparent) opacity = 0.0F;
        auto style = section.background->style();
        style.fill = resolveColorSpec(scaleAlpha(
            section.config.background.value_or(instance.barConfig.background), opacity));
        style.border = resolveColorSpec(section.config.border.value_or(instance.barConfig.border));
        style.borderWidth = section.config.borderWidth.value_or(instance.barConfig.borderWidth);
        section.background->setStyle(style);
        section.background->setMaterialPrimitive(section.config.shader == BarSectionShader::Flat
                ? std::optional{noctalia::material::Primitive::Flat}
                : section.config.shader == BarSectionShader::GlassRim
                ? std::optional{noctalia::material::Primitive::Optical} : std::nullopt);

        const float maxGrow = instance.barConfig.sectionBackgrounds
            ? std::max(0.0F, instance.barConfig.islandHoverGrow) : 0.0F;
        const float maxOffset = instance.barConfig.sectionBackgrounds
            ? std::abs(instance.barConfig.islandHoverOffset) : 0.0F;
        section.inputEnvelope->setPosition(absoluteX - (isVertical ? maxGrow + maxOffset : maxGrow),
                                           absoluteY - (isVertical ? maxGrow : maxGrow + maxOffset));
        section.inputEnvelope->setSize(width + (isVertical ? 2.0F * (maxGrow + maxOffset) : 2.0F * maxGrow),
                                       height + (isVertical ? 2.0F * maxGrow : 2.0F * (maxGrow + maxOffset)));
        section.inputEnvelope->setRadius(std::max(0.0F, static_cast<float>(instance.barConfig.radius)));
        section.inputEnvelope->setVisible(paintsOwnBackground && length > 0.0F);
        section.compactSource = {
            .section = AttachedPanelSourceSection::Unknown,
            .sectionId = section.config.id,
            // Source geometry is local to contentClip; the public provider
            // adds that node's origin exactly once.
            .x = isVertical ? section.config.crossOffset : compactExtents[i].start,
            .y = isVertical ? compactExtents[i].start : section.config.crossOffset,
            .width = isVertical ? slotCross : compactExtents[i].end - compactExtents[i].start,
            .height = isVertical ? compactExtents[i].end - compactExtents[i].start : slotCross,
            .radii = section.background->style().radius,
            .contentOffset = AttachedPanelSource::ContentOffset{
                ((isVertical ? slotCross : compactExtents[i].end - compactExtents[i].start) - section.content->width()) * 0.5F,
                ((isVertical ? compactExtents[i].end - compactExtents[i].start : slotCross) - section.content->height()) * 0.5F}};
        section.hoverSource = section.compactSource;
      }
      return;
    }

    auto configureSlot = [&](Node* slot, float mainOffset, float mainSize) {
      slot->setClipChildren(true);
      if (isVertical) {
        slot->setPosition(0.0F, mainOffset);
        slot->setSize(slotCross, mainSize);
      } else {
        slot->setPosition(mainOffset, 0.0F);
        slot->setSize(mainSize, slotCross);
      }
    };

    auto configureSection = [&](Flex* section, FlexJustify justify) {
      section->setJustify(justify);
      section->layout(renderer);
    };

    if (screenEdgeClick) {
      if (isVertical) {
        instance.startSection->setPadding(paddingInsideSection, 0.0F, 0.0F, 0.0F);
        instance.endSection->setPadding(0.0F, 0.0F, paddingInsideSection, 0.0F);
      } else {
        instance.startSection->setPadding(0.0F, 0.0F, 0.0F, paddingInsideSection);
        instance.endSection->setPadding(0.0F, paddingInsideSection, 0.0F, 0.0F);
      }
    } else {
      instance.startSection->setPadding(0.0F);
      instance.endSection->setPadding(0.0F);
    }

    configureSection(instance.startSection, FlexJustify::Start);
    configureSection(instance.centerSection, FlexJustify::Center);
    configureSection(instance.endSection, FlexJustify::End);

    // Anchor mode: if a center widget is flagged as the anchor, pin its center to the
    // bar midline so surrounding siblings growing/shrinking cannot drift it sideways.
    const Node* anchorNode = nullptr;
    for (const auto& widget : instance.centerWidgets) {
      if (widget != nullptr && widget->isAnchor() && widget->layoutBoundsNode() != nullptr) {
        anchorNode = widget->layoutBoundsNode();
        break;
      }
    }

    const float barMidline = contentMainStart + contentMainSpan * 0.5F;
    const float centerNaturalMain = isVertical ? instance.centerSection->height() : instance.centerSection->width();

    float centerSlotStart;
    float centerSlotMain;
    float centerSectionOffset; // offset of section origin within its slot along main axis
    if (anchorNode != nullptr && instance.barConfig.centerAlignment == BarCenterAlignment::Center) {
      const float anchorOffsetInSection = isVertical ? anchorNode->y() : anchorNode->x();
      const float anchorSpan = isVertical ? anchorNode->height() : anchorNode->width();
      const float anchorCenterInSection = anchorOffsetInSection + anchorSpan * 0.5F;
      // Place the section so that the anchor's center sits at barMidline.
      float desiredSectionStart = barMidline - anchorCenterInSection;
      // Clamp so the section stays within the content area.
      const float maxStart = contentMainEnd - centerNaturalMain;
      desiredSectionStart = std::clamp(desiredSectionStart, contentMainStart, std::max(contentMainStart, maxStart));
      centerSlotStart = desiredSectionStart;
      centerSlotMain = std::min(centerNaturalMain, contentMainEnd - centerSlotStart);
      centerSectionOffset = 0.0F;
    } else {
      centerSlotMain = std::min(contentMainSpan, centerNaturalMain);
      const auto alignment = instance.barConfig.centerAlignment == BarCenterAlignment::Start
          ? bar_sections::Alignment::Start
          : instance.barConfig.centerAlignment == BarCenterAlignment::End
          ? bar_sections::Alignment::End
          : bar_sections::Alignment::Center;
      centerSlotStart = bar_sections::alignedStart(
          contentMainStart, contentMainSpan, centerSlotMain, alignment);
      centerSectionOffset = (centerSlotMain - centerNaturalMain) * 0.5F;
    }
    const float centerSlotEnd = centerSlotStart + centerSlotMain;
    float startSlotMain;
    float endSlotMain;
    if (!instance.centerWidgets.empty() && (!instance.barConfig.sectionBackgrounds || centerNaturalMain > 0.0F)) {
      // Preserve the configured widget gap at section boundaries too. On
      // narrow bars an overflowing end section must not touch the clock.
      const float sectionGap = std::max(0.0F, static_cast<float>(instance.barConfig.widgetSpacing))
          + (instance.barConfig.sectionBackgrounds ? 2.0F * std::max(0.0F, padding) : 0.0F);
      startSlotMain = std::max(0.0F, centerSlotStart - contentMainStart - sectionGap);
      const float endSlotStart = std::min(contentMainEnd, centerSlotEnd + sectionGap);
      endSlotMain = std::max(0.0F, contentMainEnd - endSlotStart);
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, centerSlotStart, centerSlotMain);
      configureSlot(instance.endSlot, endSlotStart, endSlotMain);
    } else {
      // Allow start/end sections to take the full width if center is empty
      const float startNaturalMain = isVertical ? instance.startSection->height() : instance.startSection->width();
      const float endNaturalMain = isVertical ? instance.endSection->height() : instance.endSection->width();

      // Prioritize end section, because control center is likely to be there, and we don't
      // want it to be clipped by a super long start section, so the user loses access to settings.
      endSlotMain = std::min(endNaturalMain, contentMainSpan);
      const float sectionGap = instance.barConfig.sectionBackgrounds && startNaturalMain > 0 && endNaturalMain > 0
          ? std::max(0.0F, static_cast<float>(instance.barConfig.widgetSpacing)) + 2.0F * std::max(0.0F, padding)
          : 0.0F;
      startSlotMain = std::min(startNaturalMain, std::max(0.0F, contentMainSpan - endSlotMain - sectionGap));
      configureSlot(instance.startSlot, contentMainStart, startSlotMain);
      configureSlot(instance.centerSlot, contentMainStart + startSlotMain, 0.0F);
      configureSlot(instance.endSlot, contentMainEnd - endSlotMain, endSlotMain);
    }

    // Compact groups retain the center lane's anchor while bringing the two
    // satellites next to it. Use real measured widths, including section padding,
    // so font changes and narrow outputs cannot produce overlaps.
    const auto edgePolicy = instance.barConfig.centeredSections
        && instance.barConfig.edgeClusterPolicy == BarEdgeClusterPolicy::Edge
        ? BarEdgeClusterPolicy::FollowCenter
        : instance.barConfig.edgeClusterPolicy;
    if (edgePolicy == BarEdgeClusterPolicy::FollowCenter && centerSlotMain > 0.0F) {
      const float sectionGap = std::max(0, instance.barConfig.widgetSpacing)
          + (instance.barConfig.sectionBackgrounds ? 2.0F * std::max(0.0F, padding) : 0.0F);
      const float startNatural = isVertical ? instance.startSection->height() : instance.startSection->width();
      const float endNatural = isVertical ? instance.endSection->height() : instance.endSection->width();
      startSlotMain = std::min(startSlotMain, startNatural);
      endSlotMain = std::min(endSlotMain, endNatural);
      configureSlot(instance.startSlot, std::max(contentMainStart, centerSlotStart - sectionGap - startSlotMain), startSlotMain);
      configureSlot(instance.endSlot, std::min(contentMainEnd - endSlotMain, centerSlotEnd + sectionGap), endSlotMain);
    } else if (edgePolicy == BarEdgeClusterPolicy::Equidistant) {
      const float startNatural = isVertical ? instance.startSection->height() : instance.startSection->width();
      const float endNatural = isVertical ? instance.endSection->height() : instance.endSection->width();
      const auto edges = bar_sections::equidistantEdgeExtents(
          contentMainStart, contentMainSpan, startNatural, endNatural);
      startSlotMain = edges[0].end - edges[0].start;
      endSlotMain = edges[1].end - edges[1].start;
      configureSlot(instance.startSlot, edges[0].start, startSlotMain);
      configureSlot(instance.endSlot, edges[1].start, endSlotMain);
    }

    if (isVertical) {
      instance.startSection->setPosition((slotCross - instance.startSection->width()) * 0.5F, 0.0F);
      instance.centerSection->setPosition((slotCross - instance.centerSection->width()) * 0.5F, centerSectionOffset);
      instance.endSection->setPosition(
          (slotCross - instance.endSection->width()) * 0.5F, endSlotMain - instance.endSection->height()
      );
    } else {
      instance.startSection->setPosition(0.0F, (slotCross - instance.startSection->height()) * 0.5F);
      instance.centerSection->setPosition(centerSectionOffset, (slotCross - instance.centerSection->height()) * 0.5F);
      instance.endSection->setPosition(
          endSlotMain - instance.endSection->width(), (slotCross - instance.endSection->height()) * 0.5F
      );
    }

    // Capture the ordinary layout before panel reflow changes slots or visibility.
    instance.compactPanelSources = {};
    if (instance.barConfig.sectionBackgrounds && instance.bg != nullptr) {
      std::array<bar_sections::Extent, 3> compactExtents{};
      for (std::size_t i = 0; i < sectionNodes.size(); ++i) {
        const auto* section = sectionNodes[i];
        const auto* slot = slotNodes[i];
        if (section == nullptr || slot == nullptr) continue;
        const bool visible = std::ranges::any_of(section->children(), [](const auto& child) {
          return child->visible() && child->participatesInLayout() && child->width() > 0 && child->height() > 0;
        });
        const float slotStart = isVertical ? slot->y() : slot->x();
        const float slotEnd = slotStart + (isVertical ? slot->height() : slot->width());
        const float start = slotStart + (isVertical ? section->y() : section->x());
        const float end = start + (isVertical ? section->height() : section->width());
        compactExtents[i] = bar_sections::clippedExtent(slotStart, slotEnd, start, end, visible);
      }
      const auto padded = bar_sections::paddedExtents(compactExtents, mainSpan, padding,
          static_cast<float>(instance.barConfig.widgetSpacing));
      for (std::size_t i = 0; i < padded.size(); ++i) {
        if (!padded[i].visible) continue;
        const auto rect = bar_sections::rectangle(padded[i], slotCross, isVertical);
        instance.compactPanelSources[i] = {
            .section = static_cast<AttachedPanelSourceSection>(i + 1),
            .x = rect.x, .y = rect.y, .width = rect.width, .height = rect.height,
            .radii = instance.bg->style().radius,
            .contentOffset = AttachedPanelSource::ContentOffset{
                slotNodes[i]->x() + sectionNodes[i]->x() - rect.x,
                slotNodes[i]->y() + sectionNodes[i]->y() - rect.y}};
      }
    }

    if (instance.barConfig.sectionBackgrounds && instance.barConfig.islandHoverGrow > 0.F
        && !instance.attachedPanelGeometry.has_value()) {
      auto sources = instance.compactPanelSources;
      std::array<bar_island_morph::Extent, 3> extents{};
      std::array<float, 3> progress{};
      const std::array<const std::vector<std::unique_ptr<Widget>>*, 3> groups{
          &instance.startWidgets, &instance.centerWidgets, &instance.endWidgets};
      for (std::size_t i = 0; i < sources.size(); ++i) {
        const auto& source = sources[i];
        const float start = isVertical ? source.y : source.x;
        extents[i] = {start, start + (isVertical ? source.height : source.width), source.valid()};
        for (const auto& widget : *groups[i]) progress[i] = std::max(progress[i], widget->barHoverProgress());
      }
      for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i].valid()) continue;
        const float length = extents[i].end - extents[i].start;
        const float growth = instance.barConfig.islandHoverGrow * progress[i] * (length > slotCross * 1.5F ? 2.F : 1.F);
        const auto target = bar_island_morph::Extent{extents[i].start - growth / 2.F, extents[i].end + growth / 2.F, true};
        extents = bar_island_morph::reflow(extents, i, target, 1.F, mainSpan,
            static_cast<float>(instance.barConfig.widgetSpacing)).extents;
      }
      const bool reverse = instance.barConfig.position == "bottom" || instance.barConfig.position == "right";
      for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i].valid()) continue;
        const float oldStart = isVertical ? sources[i].y : sources[i].x;
        const float oldLength = isVertical ? sources[i].height : sources[i].width;
        const float length = extents[i].end - extents[i].start;
        const float crossGrow = instance.barConfig.islandHoverGrow * progress[i];
        const float offset = instance.barConfig.islandHoverOffset * progress[i];
        const float crossStart = reverse ? -offset - crossGrow : offset;
        const float sectionLength = isVertical ? sectionNodes[i]->height() : sectionNodes[i]->width();
        const float sectionCross = isVertical ? sectionNodes[i]->width() : sectionNodes[i]->height();
        auto* slot = slotNodes[i]; auto* section = sectionNodes[i];
        if (isVertical) {
          slot->setPosition(crossStart, extents[i].start); slot->setSize(slotCross + crossGrow, length);
          section->setPosition((slotCross + crossGrow - sectionCross) / 2.F, (length - sectionLength) / 2.F);
          sources[i].x = crossStart; sources[i].y = extents[i].start;
          sources[i].width = slotCross + crossGrow; sources[i].height = length;
        } else {
          slot->setPosition(extents[i].start, crossStart); slot->setSize(length, slotCross + crossGrow);
          section->setPosition((length - sectionLength) / 2.F, (slotCross + crossGrow - sectionCross) / 2.F);
          sources[i].x = extents[i].start; sources[i].y = crossStart;
          sources[i].width = length; sources[i].height = slotCross + crossGrow;
        }
        (void)oldStart; (void)oldLength;
        sources[i].contentOffset = AttachedPanelSource::ContentOffset{section->x(), section->y()};
      }
      instance.hoveredPanelSources = sources;
      instance.compactPanelSources = sources;
    }

    if (usesStableIslandMorphSurface(instance.barConfig) && instance.attachedPanelGeometry.has_value()) {
      const auto& attached = *instance.attachedPanelGeometry;
      const auto active = sourceSectionIndex(attached.source.section);
      if (active.has_value() && attached.source.valid()) {
        std::array<bar_sections::Extent, 3> contentExtents{};
        for (std::size_t index = 0; index < sectionNodes.size(); ++index) {
          const auto* section = sectionNodes[index];
          const auto* slot = slotNodes[index];
          if (section == nullptr || slot == nullptr) continue;
          const bool visibleContent = std::ranges::any_of(section->children(), [](const auto& child) {
            return child->visible() && child->participatesInLayout() && child->width() > 0.0F
                && child->height() > 0.0F;
          });
          const float slotStart = isVertical ? slot->y() : slot->x();
          const float sectionStart = slotStart + (isVertical ? section->y() : section->x());
          const float sectionExtent = isVertical ? section->height() : section->width();
          const float slotEnd = slotStart + (isVertical ? slot->height() : slot->width());
          contentExtents[index] = bar_sections::clippedExtent(
              slotStart, slotEnd, sectionStart, sectionStart + sectionExtent, visibleContent);
        }
        const auto padded = bar_sections::paddedExtents(
            contentExtents, mainSpan, std::max(0.0F, padding),
            static_cast<float>(std::max(0, instance.barConfig.widgetSpacing))
        );
        std::array<bar_island_morph::Extent, 3> baseline{};
        for (std::size_t index = 0; index < baseline.size(); ++index) {
          baseline[index] = {padded[index].start, padded[index].end, padded[index].visible};
        }
        const float clipMainOrigin = isVertical ? instance.contentClip->y() : instance.contentClip->x();
        const float targetStart =
            (isVertical ? attached.finalOutputY : attached.finalOutputX) - clipMainOrigin;
        const float targetExtent = isVertical ? attached.finalOutputHeight : attached.finalOutputWidth;
        const auto morphed = bar_island_morph::reflow(
            baseline, *active, {targetStart, targetStart + targetExtent, targetExtent > 0.0F},
            attached.revealProgress,
            mainSpan, static_cast<float>(std::max(0, instance.barConfig.islandMorphGap))
        );
        instance.islandMorphExtents = morphed.extents;

        for (std::size_t index = 0; index < sectionNodes.size(); ++index) {
          auto* section = sectionNodes[index];
          auto* slot = slotNodes[index];
          if (section == nullptr || slot == nullptr || !morphed.extents[index].visible) continue;
          const float oldSlotStart = isVertical ? slot->y() : slot->x();
          const float oldSectionStart = oldSlotStart + (isVertical ? section->y() : section->x());
          const float delta = morphed.extents[index].start - baseline[index].start;
          configureSlot(
              slot, morphed.extents[index].start,
              std::max(0.0F, morphed.extents[index].end - morphed.extents[index].start)
          );
          // Keep the opener content at its compact absolute position until the
          // panel takes ownership. Start/end growth otherwise drags media/status
          // to the new outer edge instead of growing around the retained opener.
          const float sectionStart = index == *active ? oldSectionStart : oldSectionStart + delta;
          const float relativeSectionStart = sectionStart - morphed.extents[index].start;
          if (isVertical) {
            section->setPosition((slotCross - section->width()) * 0.5F, relativeSectionStart);
          } else {
            section->setPosition(relativeSectionStart, (slotCross - section->height()) * 0.5F);
          }
        }
        if (attached.panelOwnsSource) {
          sectionNodes[*active]->setOpacity(0.F);
          sectionNodes[*active]->setHitTestVisible(false);
        }
      }
    }

    const float stableHoverEnvelope = instance.barConfig.sectionBackgrounds
        ? std::max(0.0F, instance.barConfig.islandHoverOffset)
            + std::max(0.0F, instance.barConfig.islandHoverGrow)
        : 0.0F;
    applyBarWidgetHitTargets(instance.startSection, instance.startSlot, isVertical, stableHoverEnvelope);
    applyBarWidgetHitTargets(instance.centerSection, instance.centerSlot, isVertical, stableHoverEnvelope);
    applyBarWidgetHitTargets(instance.endSection, instance.endSlot, isVertical, stableHoverEnvelope);
    extendCapsuleHitTargets(instance.startCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);
    extendCapsuleHitTargets(instance.centerCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);
    extendCapsuleHitTargets(instance.endCapsuleRuns, isVertical, instance.barConfig.widgetCapsulePadding);
    syncCapsuleContourHitGeometry(instance.startCapsuleRuns, isVertical);
    syncCapsuleContourHitGeometry(instance.centerCapsuleRuns, isVertical);
    syncCapsuleContourHitGeometry(instance.endCapsuleRuns, isVertical);

    // Ghost pills for capsule-less widgets: positioned on the bar-level underlay with the
    // metrics an enabled capsule would have (capsuleCross across the bar, capsule padding
    // along it). Runs after sections are positioned so absolute coordinates are final; the
    // widget's hit target is widened to match the pill.
    if (instance.hoverUnderlay != nullptr) {
      float underlayX = 0.0F;
      float underlayY = 0.0F;
      Node::absolutePosition(instance.hoverUnderlay, underlayX, underlayY);
      auto placeGhostPills = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
        for (auto& widget : widgets) {
          Box* box = widget->barHoverBox();
          Node* root = widget->outerNode();
          if (box == nullptr || root == nullptr || widget->barCapsuleShell() != nullptr) {
            continue;
          }
          if (!root->visible() || !root->participatesInLayout()) {
            box->setSize(0.0F, 0.0F);
            continue;
          }
          float rootX = 0.0F;
          float rootY = 0.0F;
          Node::absolutePosition(root, rootX, rootY);
          rootX -= underlayX;
          rootY -= underlayY;
          const float mainExtent = isVertical ? root->height() : root->width();
          const float pad = mainExtent > 0.5F ? widget->barCapsuleSpec().padding * widget->contentScale() : 0.0F;
          const float hoverW = isVertical ? capsuleCross : root->width() + 2.0F * pad;
          const float hoverH = isVertical ? root->height() + 2.0F * pad : capsuleCross;
          box->setPosition(
              isVertical ? rootX + (root->width() - capsuleCross) * 0.5F : rootX - pad,
              isVertical ? rootY - pad : rootY + (root->height() - capsuleCross) * 0.5F
          );
          box->setSize(hoverW, hoverH);
          box->setRadius(widget->resolvedBarCapsuleRadius(hoverW, hoverH));
          if (auto* area = dynamic_cast<InputArea*>(root)) {
            auto outset = area->hitTestOutset();
            if (isVertical) {
              outset.top += pad;
              outset.bottom += pad;
            } else {
              outset.left += pad;
              outset.right += pad;
            }
            area->setHitTestOutset(outset);
          }
        }
      };
      placeGhostPills(instance.startWidgets);
      placeGhostPills(instance.centerWidgets);
      placeGhostPills(instance.endWidgets);

      auto placeGroupHoverPills = [&](std::vector<BarCapsuleRun>& runs) {
        for (auto& run : runs) {
          if (run.container == nullptr || run.shell == nullptr) {
            continue;
          }
          float shellX = 0.0F;
          float shellY = 0.0F;
          Node::absolutePosition(run.shell, shellX, shellY);
          shellX -= underlayX;
          shellY -= underlayY;
          const float shellMainStart = isVertical ? shellY : shellX;
          const Widget* radiusSource = !run.widgets.empty() ? run.widgets.front() : nullptr;
          for (Widget* widget : run.widgets) {
            Node* root = widget != nullptr ? widget->outerNode() : nullptr;
            Box* box = widget != nullptr ? widget->barHoverBox() : nullptr;
            if (box != nullptr
                && (root == nullptr || !root->visible() || !root->participatesInLayout() || !root->hitTestVisible())) {
              box->setSize(0.0F, 0.0F);
            }
          }

          const auto laidOut = capsuleMemberSlices(run, isVertical, instance.barConfig.widgetCapsulePadding);
          for (const auto& member : laidOut) {
            Widget* widget = run.widgets[member.widgetIndex];
            Box* box = widget->barHoverBox();
            if (box == nullptr) {
              continue;
            }

            const float rootX = shellX + member.x;
            const float rootY = shellY + member.y;
            const float mainStart = shellMainStart + member.sliceStart;
            const float mainEnd = shellMainStart + member.sliceEnd;
            const float mainExtent = std::max(0.0F, mainEnd - mainStart);
            const float hoverW = isVertical ? capsuleCross : mainExtent;
            const float hoverH = isVertical ? mainExtent : capsuleCross;
            box->setPosition(
                isVertical ? rootX + (member.width - capsuleCross) * 0.5F : mainStart,
                isVertical ? mainStart : rootY + (member.height - capsuleCross) * 0.5F
            );
            box->setSize(hoverW, hoverH);
            box->setRadius(
                radiusSource != nullptr ? radiusSource->resolvedBarCapsuleRadius(hoverW, hoverH)
                                        : widget->resolvedBarCapsuleRadius(hoverW, hoverH)
            );
          }
        }
      };
      placeGroupHoverPills(instance.startCapsuleRuns);
      placeGroupHoverPills(instance.centerCapsuleRuns);
      placeGroupHoverPills(instance.endCapsuleRuns);
    }
    if (screenEdgeClick) {
      auto extendHitTestOutsetToScreenEdge = [&](Node* node, bool start) {
        if (node == nullptr) {
          return;
        }
        auto hitTestOutset = node->hitTestOutset();
        if (start) {
          if (isVertical) {
            hitTestOutset.top += paddingInsideSection;
          } else {
            hitTestOutset.left += paddingInsideSection;
          }
        } else {
          if (isVertical) {
            hitTestOutset.bottom += paddingInsideSection;
          } else {
            hitTestOutset.right += paddingInsideSection;
          }
        }
        node->setHitTestOutset(hitTestOutset);
      };
      if (!instance.startWidgets.empty()) {
        auto* widget = instance.startWidgets.front().get();
        extendHitTestOutsetToScreenEdge(widget->outerNode(), true);
        extendHitTestOutsetToScreenEdge(widget->root(), true);
      }
      if (!instance.endWidgets.empty()) {
        auto* widget = instance.endWidgets.back().get();
        extendHitTestOutsetToScreenEdge(widget->outerNode(), false);
        extendHitTestOutsetToScreenEdge(widget->root(), false);
      }
    }
    layoutBarSectionBackgrounds(instance);
  }

  void tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) {
    for (auto& widget : widgets) {
      if (widget != nullptr && widget->needsFrameTick()) {
        widget->onFrameTick(deltaMs);
      }
    }
  }

  bool widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
    return std::ranges::any_of(widgets, [](const auto& widget) {
      return widget != nullptr && widget->needsFrameTick();
    });
  }

} // namespace

Bar::Bar() = default;

bool Bar::initialize(const BarServices& services) {
  m_platform = &services.platform;
  m_config = &services.config;
  m_notifications = services.notifications;
  m_tray = services.tray;
  m_audio = services.audio;
  m_easyEffects = services.easyEffects;
  m_upower = services.upower;
  m_sysmon = services.sysmon;
  m_powerProfiles = services.powerProfiles;
  m_network = services.network;
  m_idleInhibitor = services.idleInhibitor;
  m_mpris = services.mpris;
  m_audioSpectrum = services.audioSpectrum;
  m_httpClient = services.httpClient;
  m_weatherService = services.weather;
  m_renderContext = services.renderContext;
  m_nightLight = services.nightLight;
  m_themeService = services.theme;
  m_bluetooth = services.bluetooth;
  m_brightness = services.brightness;
  m_lockKeys = services.lockKeys;
  m_clipboard = services.clipboard;
  m_fileWatcher = services.fileWatcher;
  m_screenshots = services.screenshots;
  m_scriptApi = services.scriptApi;

  m_widgetFactory = std::make_unique<WidgetFactory>(services);

  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  syncAppearanceSnapshot();
  m_lastPlugins = m_config->config().plugins;
  m_config->addReloadCallback(
      [this]() {
        const auto& cfg = m_config->config();
        const bool appearanceChanged=syncAppearanceSnapshot();
        if (cfg.bars==m_lastBars && cfg.widgets==m_lastWidgets && cfg.plugins==m_lastPlugins) {
          if(appearanceChanged || cfg.shell.shadow!=m_lastShadow) {
            m_lastShadow=cfg.shell.shadow;
            refreshSurfaceGeometry();
          }
          return;
        }
        reload();
      },
      "bar"
  );

  return true;
}

BarServices Bar::services() const {
  return {
      .platform = *m_platform,
      .config = *m_config,
      .notifications = m_notifications,
      .tray = m_tray,
      .audio = m_audio,
      .easyEffects = m_easyEffects,
      .upower = m_upower,
      .sysmon = m_sysmon,
      .powerProfiles = m_powerProfiles,
      .network = m_network,
      .idleInhibitor = m_idleInhibitor,
      .mpris = m_mpris,
      .audioSpectrum = m_audioSpectrum,
      .httpClient = m_httpClient,
      .weather = m_weatherService,
      .renderContext = m_renderContext,
      .nightLight = m_nightLight,
      .theme = m_themeService,
      .bluetooth = m_bluetooth,
      .brightness = m_brightness,
      .lockKeys = m_lockKeys,
      .clipboard = m_clipboard,
      .fileWatcher = m_fileWatcher,
      .screenshots = m_screenshots,
      .scriptApi = m_scriptApi,
  };
}

void Bar::onSecondTick() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
    }
  }
}

bool Bar::syncAppearanceSnapshot() {
  const auto& cfg=m_config->config();
  const bool changed=m_lastMaterial!=cfg.shell.material || m_lastMaterialOverrides!=cfg.shell.materialOverrides
      || m_lastDesign!=cfg.shell.design || m_lastSurfaceMaterial!=cfg.shell.surfaceMaterial
      || m_lastCornerScale!=cfg.shell.cornerRadiusScale || m_lastUiScale!=cfg.accessibility.uiScale;
  m_lastMaterial=cfg.shell.material;m_lastMaterialOverrides=cfg.shell.materialOverrides;
  m_lastDesign=cfg.shell.design;m_lastSurfaceMaterial=cfg.shell.surfaceMaterial;
  m_lastCornerScale=cfg.shell.cornerRadiusScale;m_lastUiScale=cfg.accessibility.uiScale;
  return changed;
}

void Bar::refreshSurfaceGeometry() {
  for(auto& instance:m_instances) {
    if(!instance->surface)continue;
    const auto spec=computeBarSurfaceSpec(instance->barConfig,m_config->config().shell.shadow);
    instance->surface->setMargins(spec.marginTop,spec.marginRight,spec.marginBottom,spec.marginLeft);
    instance->surface->requestSize(spec.surfaceWidth,spec.surfaceHeight);
    syncBarSurfaceChrome(*instance);
    // buildScene updates the retained chrome when sceneRoot already exists.
    // Widgets, their models, focus, and open popup ownership remain intact.
    buildScene(*instance,instance->surface->width(),instance->surface->height());
    instance->surface->requestUpdate();
  }
}

void Bar::reload() {
  syncAppearanceSnapshot();
  noctalia::profiling::ScopedTimer t(kLog, "bar: reload (all instances)");
  kLog.info("reloading config");
  const auto previousBars = m_lastBars;
  const auto previousShadow = m_lastShadow;
  const bool recreateForOrder = barSurfaceOrderRequiresRecreate(previousBars, m_config->config().bars);
  m_lastBars = m_config->config().bars;
  m_lastWidgets = m_config->config().widgets;
  m_lastShadow = m_config->config().shell.shadow;
  m_lastPlugins = m_config->config().plugins;
  m_widgetFactory = std::make_unique<WidgetFactory>(services());

  if (recreateForOrder) {
    kLog.info("bar order changed; recreating layer-shell surfaces");
    closeAllInstances();
    if (wl_display_roundtrip(m_platform->display()) < 0) {
      const int roundtripErrno = errno;
      kLog.error(
          "Wayland roundtrip failed after destroying bar surfaces for order change: {}",
          m_platform->wayland().describeDisplayError(roundtripErrno)
      );
    }
    syncInstances();
    return;
  }

  // Look up new bar configs by name.
  std::unordered_map<std::string, std::pair<const BarConfig*, std::size_t>> newBarsByName;
  newBarsByName.reserve(m_lastBars.size());
  for (std::size_t i = 0; i < m_lastBars.size(); ++i) {
    newBarsByName[m_lastBars[i].name] = {&m_lastBars[i], i};
  }

  // Exclusive-zone geometry on an output depends on the order its bar surfaces are
  // created: bars on the same edge stack in creation order, and bars on adjacent
  // edges (e.g. top + left) compete for the shared corner the same way. Rebuilding
  // one bar's surface in place while recreating another would commit them out of
  // config order and reshuffle that geometry. So if any bar on an output needs a
  // surface recreate, recreate every bar on that output — syncInstances rebuilds
  // them in config order. Scoped per output so other monitors are untouched.
  const auto needsSurfaceRecreate = [&](const BarInstance& inst) -> bool {
    auto it = newBarsByName.find(inst.barConfig.name);
    if (it == newBarsByName.end()) {
      return true;
    }
    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return true;
    }
    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return true;
    }
    return !barConfigSurfaceFieldsEqual(inst.barConfig, resolved, previousShadow, m_lastShadow);
  };
  std::unordered_set<std::uint32_t> outputsNeedingRecreate;
  for (const auto& instUp : m_instances) {
    if (needsSurfaceRecreate(*instUp)) {
      outputsNeedingRecreate.insert(instUp->outputName);
    }
  }

  // For each existing instance, decide whether to rebuild contents in place
  // (surface preserved → no exclusive-zone churn) or destroy (will be recreated
  // by syncInstances below). Any bar on an output flagged above is destroyed so the
  // whole output is rebuilt in config order.
  bool destroyedAny = false;
  std::erase_if(m_instances, [&](const std::unique_ptr<BarInstance>& instUp) {
    auto& inst = *instUp;
    auto it = newBarsByName.find(inst.barConfig.name);
    auto destroy = [&]() {
      if (inst.surface != nullptr) {
        m_surfaceMap.erase(inst.surface->wlSurface());
      }
      if (m_hoveredInstance == &inst) {
        m_hoveredInstance = nullptr;
      }
      destroyedAny = true;
      return true;
    };
    if (outputsNeedingRecreate.contains(inst.outputName)) {
      return destroy();
    }
    if (it == newBarsByName.end()) {
      return destroy();
    }

    const auto& outputs = m_platform->outputs();
    auto outIt = std::ranges::find(outputs, inst.outputName, &WaylandOutput::name);
    if (outIt == outputs.end()) {
      return destroy();
    }

    auto resolved = ConfigService::resolveForOutput(*it->second.first, *outIt);
    if (!resolved.enabled) {
      return destroy();
    }

    inst.barIndex = it->second.second;
    rebuildInstanceContents(inst, resolved);
    return false;
  });

  if (destroyedAny) {
    // Drain pending Wayland events for the just-destroyed surfaces before
    // creating new ones. Without this, the roundtrip inside LayerSurface::initialize
    // reads stale closures for dead proxies, which libwayland drops without freeing.
    if (wl_display_roundtrip(m_platform->display()) < 0) {
      const int roundtripErrno = errno;
      kLog.error(
          "Wayland roundtrip failed after destroying stale bar surfaces: {}",
          m_platform->wayland().describeDisplayError(roundtripErrno)
      );
    }
  }

  syncInstances();
}

void Bar::closeAllInstances() {
  m_surfaceMap.clear();
  m_hoveredInstance = nullptr;
  m_instances.clear();
}

void Bar::onOutputChange() { syncInstances(); }

void Bar::onWorkspaceChanged() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  bool anyChanged = false;
  for (const auto& output : m_platform->outputs()) {
    const std::string activeId = activeWorkspaceId(m_platform->workspaces(output.output));
    if (activeId.empty()) {
      continue;
    }

    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (!last.empty() && last != activeId) {
      m_pendingWorkspaceRevealOutputs.insert(output.name);
      anyChanged = true;
    }
    last = activeId;
  }

  if (anyChanged) {
    m_workspaceRevealDebounce.start(kWorkspaceRevealDebounce, [this]() { applyPendingWorkspaceReveal(); });
  }
  scheduleSmartAutoHideReevaluation();
}

void Bar::scheduleSmartAutoHideReevaluation() {
  if (m_smartAutoHideReevalQueued) {
    return;
  }
  m_smartAutoHideReevalQueued = true;
  DeferredCall::callLater([this]() {
    m_smartAutoHideReevalQueued = false;
    reevaluateSmartAutoHide();
  });
}

void Bar::reevaluateSmartAutoHide() {
  if (m_platform == nullptr || m_overlayDisplaySuppressed) {
    return;
  }

  for (const auto& instanceUp : m_instances) {
    BarInstance* instance = instanceUp.get();
    if (instance == nullptr
        || !instance->barConfig.enabled
        || !instance->barConfig.smartAutoHide
        || instance->surface == nullptr) {
      continue;
    }

    const bool wantsPinned = smartAutoHideWantsPinnedVisible(*m_platform, instance->output);
    const bool pinnedChanged = wantsPinned != instance->smartAutoHidePinnedVisible;
    instance->smartAutoHidePinnedVisible = wantsPinned;

    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;

    bool needsRedraw = pinnedChanged;
    if (wantsPinned) {
      if (instance->hideOpacity < 1.0F || pinnedChanged) {
        revealAutoHideBar(*instance);
        syncBarAutoHideInputRegion(*instance);
        syncBarSurfaceChrome(*instance);
        needsRedraw = true;
      }
    } else if (!instance->pointerInside && instance->attachedPopupCount == 0 && !suppressAutoHide) {
      if ((instance->hideOpacity > 0.0F || pinnedChanged)
          && (!instance->barConfig.showOnWorkspaceSwitch || !isWorkspacePeekActive())) {
        startHideFadeOut(*instance);
        needsRedraw = true;
      }
    }

    if (needsRedraw && instance->surface != nullptr) {
      instance->surface->requestRedraw();
    }
  }
}

bool Bar::isWorkspacePeekActive() const noexcept {
  return m_workspaceRevealDebounce.active() || m_workspacePeekHideTimer.active();
}

void Bar::applyPendingWorkspaceReveal() {
  if (m_platform == nullptr) {
    return;
  }

  const auto pendingOutputs = std::move(m_pendingWorkspaceRevealOutputs);
  m_pendingWorkspaceRevealOutputs.clear();

  std::vector<BarInstance*> peeked;
  peeked.reserve(m_instances.size());
  for (const std::uint32_t outputName : pendingOutputs) {
    for (const auto& instanceUp : m_instances) {
      auto* instance = instanceUp.get();
      if (instance == nullptr
          || instance->outputName != outputName
          || !instance->barConfig.enabled
          || !instance->barConfig.isAutoHideEnabled()
          || !instance->barConfig.showOnWorkspaceSwitch
          || instance->surface == nullptr) {
        continue;
      }

      revealAutoHideBar(*instance);
      if (instance->pointerInside) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        peeked.push_back(instance);
      }
    }
  }

  if (peeked.empty()) {
    return;
  }

  m_workspacePeekHideTimer.start(kWorkspacePeekHold, [this, peeked = std::move(peeked)]() {
    for (BarInstance* instance : peeked) {
      if (instance == nullptr || !instance->barConfig.isAutoHideEnabled() || instance->pointerInside) {
        continue;
      }
      if (instance->barConfig.smartAutoHide && instance->smartAutoHidePinnedVisible) {
        continue;
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
      if (!suppressAutoHide) {
        startHideFadeOut(*instance);
      }
    }
  });
}

void Bar::refresh() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
      if (inst->animations.hasActive() || instanceNeedsFrameTick(*inst)) {
        inst->surface->requestRedraw();
      }
    }
  }
}

void Bar::requestRedraw() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestRedraw();
    }
  }
}

void Bar::requestLayout() {
  for (auto& inst : m_instances) {
    if (inst->surface != nullptr) {
      inst->surface->requestLayout();
    }
  }
}

void Bar::setAutoHideSuppressionCallback(std::function<bool(const BarInstance&)> callback) {
  m_autoHideSuppressionCallback = std::move(callback);
}

void Bar::reevaluateAutoHide() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr
        || !barPointerHideAllowed(*instance)
        || instance->pointerInside
        || instance->attachedPopupCount > 0) {
      continue;
    }
    const bool suppressAutoHide =
        (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
    if (suppressAutoHide || instance->hideOpacity <= 0.001F) {
      continue;
    }
    startHideFadeOut(*instance);
  }
}

void Bar::rearmTooltipForHoveredWidget() {
  if (m_hoveredInstance == nullptr || !m_hoveredInstance->pointerInside) {
    return;
  }
  auto* hovered = m_hoveredInstance->inputDispatcher.hoveredArea();
  if (hovered == nullptr || m_hoveredInstance->surface == nullptr) {
    return;
  }
  // Same path a real hover takes, so the usual show delay still applies — the
  // tooltip must not blink into existence the instant the panel disappears.
  TooltipManager::instance().onBarHoverChange(
      hovered, m_hoveredInstance->surface->layerSurface(), m_hoveredInstance->output
  );
}

void Bar::reevaluateAutoHideAfterPopup() {
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    wl_surface* const surface = instance->surface->wlSurface();
    instance->pointerInside = m_platform != nullptr
        && surface != nullptr
        && m_platform->hasPointerPosition()
        && m_platform->lastPointerSurface() == surface;
    if (instance->pointerInside) {
      instance->lastPointerSx = static_cast<float>(m_platform->lastPointerX());
      instance->lastPointerSy = static_cast<float>(m_platform->lastPointerY());
      instance->inputDispatcher.pointerEnter(
          instance->lastPointerSx, instance->lastPointerSy, m_platform->lastInputSerial()
      );
      m_hoveredInstance = instance.get();
    } else {
      instance->inputDispatcher.pointerLeave();
      if (m_hoveredInstance == instance.get()) {
        m_hoveredInstance = nullptr;
      }
    }
    instance->surface->requestRedraw();
  }
  reevaluateAutoHide();
}

bool Bar::isRunning() const noexcept {
  return std::ranges::any_of(m_instances, [](const auto& inst) { return inst->surface && inst->surface->isRunning(); });
}

bool Bar::instanceEffectivelyVisible(const BarInstance& instance) const noexcept {
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > 0.5F;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > 0.5F;
}

bool Bar::instanceAcceptsPointerInput(const BarInstance& instance) const noexcept {
  return barSupportsSlideBehavior(instance.barConfig) || !instance.ipcLayoutReleased;
}

bool Bar::isVisible() const noexcept {
  return std::ranges::any_of(m_instances, [this](const auto& inst) { return instanceEffectivelyVisible(*inst); });
}

void Bar::clearInstancePointerState(BarInstance& instance) {
  instance.pointerInside = false;
  instance.inputDispatcher.pointerLeave();
  if (m_hoveredInstance == &instance) {
    m_hoveredInstance = nullptr;
  }
}

void Bar::setInstanceIpcVisible(BarInstance& instance, bool visible) {
  if (instance.surface == nullptr) {
    return;
  }
  if (barSupportsSlideBehavior(instance.barConfig)) {
    if (visible) {
      revealAutoHideBar(instance);
    } else {
      startHideFadeOut(instance);
    }
    return;
  }
  if (instance.slideRoot == nullptr) {
    return;
  }
  instance.animations.cancelForOwner(instance.slideRoot);
  instance.slideRoot->setOpacity(1.0F);
  if (!visible) {
    clearInstancePointerState(instance);
  }
  const float current = instance.hideOpacity;
  const float target = visible ? 1.0F : 0.0F;
  instance.animations.animate(
      current, target, Style::animNormal, visible ? Easing::EaseOutCubic : Easing::EaseInQuad,
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this]() {
        syncBarSlideLayerTransform(*inst);
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        if (inst->surface != nullptr) {
          inst->surface->requestRedraw();
        }
      },
      instance.slideRoot
  );
  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::applyIpcVisibility(bool visible) {
  for (const auto& instance : m_instances) {
    if (instance == nullptr) {
      continue;
    }
    setInstanceIpcVisible(*instance, visible);
    syncBarSurfaceChrome(*instance);
  }
  syncIdleInhibitorAnchors();
}

void Bar::syncIdleInhibitorAnchors() {
  if (m_idleInhibitor != nullptr) {
    m_idleInhibitor->resyncAnchorSurfaces();
  }
}

bool Bar::barContentVisuallyShown(const BarInstance& instance) const noexcept {
  constexpr float kShownThreshold = 0.02F;
  if (barSupportsSlideBehavior(instance.barConfig)) {
    return instance.hideOpacity > kShownThreshold;
  }
  return instance.slideRoot == nullptr || instance.hideOpacity > kShownThreshold;
}

bool Bar::shouldReserveExclusiveZone(const BarInstance& instance) const noexcept {
  if (instance.ipcLayoutReleased) {
    return false;
  }
  return instance.barConfig.reserveSpace;
}

void Bar::syncBarExclusiveZone(BarInstance& instance) {
  if (instance.surface == nullptr || m_config == nullptr) {
    return;
  }
  const std::int32_t zone = shouldReserveExclusiveZone(instance)
      ? reservedBarExclusiveZone(instance.barConfig, m_config->config().shell.shadow)
      : 0;
  instance.surface->setExclusiveZone(zone);
}

void Bar::syncBarSurfaceChrome(BarInstance& instance) {
  syncBarExclusiveZone(instance);
  if (instance.barConfig.sectionBackgrounds) syncBarAutoHideInputRegion(instance);
  applyBarCompositorBlur(instance);
  const auto syncRing = [&](const std::vector<std::unique_ptr<Widget>>& widgets, Box* background,
                            bool panelOwns) {
    if (!background) return;
    Widget* only = nullptr;
    for (const auto& widget : widgets) {
      if (!widget->outerNode() || !widget->outerNode()->visible() || !widget->outerNode()->participatesInLayout()) continue;
      if (only) return; // grouped modules retain their individual widget rings
      only = widget.get();
    }
    if (!only || !only->usageRingStyle()) return;
    float bx = 0, by = 0, wx = 0, wy = 0;
    Node::absolutePosition(background, bx, by);
    Node::absolutePosition(only->outerNode(), wx, wy);
    only->setUsageRingFrame(bx - wx, by - wy, background->width(), background->height(),
        background->style().radius.tl, background->visible() && !panelOwns);
  };
  for (auto& section : instance.dynamicSections) {
    const bool owned = instance.attachedPanelGeometry && instance.attachedPanelGeometry->panelOwnsSource
        && instance.attachedPanelGeometry->source.sectionId == section.config.id;
    syncRing(section.widgets, section.background, owned);
  }
  if (instance.barConfig.sectionBackgrounds && instance.dynamicSections.empty()) {
    const std::array<const std::vector<std::unique_ptr<Widget>>*, 3> groups{
        &instance.startWidgets, &instance.centerWidgets, &instance.endWidgets};
    for (std::size_t i = 0; i < groups.size(); ++i) {
      const bool owned = instance.attachedPanelGeometry && instance.attachedPanelGeometry->panelOwnsSource
          && sourceSectionIndex(instance.attachedPanelGeometry->source.section) == i;
      syncRing(*groups[i], instance.sectionBackgrounds[i], owned);
    }
  }
}

std::optional<LayerPopupParentContext> Bar::popupParentContextForSurface(wl_surface* surface) const noexcept {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr || instance->surface == nullptr) {
    return std::nullopt;
  }

  auto* layerSurface = instance->surface->layerSurface();
  const auto width = instance->surface->width();
  const auto height = instance->surface->height();
  if (layerSurface == nullptr || width == 0 || height == 0) {
    return std::nullopt;
  }

  return LayerPopupParentContext{
      .surface = instance->surface->wlSurface(),
      .layerSurface = layerSurface,
      .output = instance->output,
      .width = width,
      .height = height,
      .materialSurface = "bar",
  };
}

std::optional<LayerPopupParentContext> Bar::preferredPopupParentContext(wl_output* output) const noexcept {
  BarInstance* instance = instanceForOutput(output);
  if (instance == nullptr && !m_instances.empty()) {
    instance = m_instances.front().get();
  }
  return instance != nullptr && instance->surface != nullptr
      ? popupParentContextForSurface(instance->surface->wlSurface())
      : std::nullopt;
}

std::vector<InputRect> Bar::surfaceRectsForOutput(wl_output* output) const {
  std::vector<InputRect> rects;
  if (m_platform == nullptr || output == nullptr) {
    return rects;
  }

  const WaylandOutput* wlOutput = m_platform->findOutputByWl(output);
  if (wlOutput == nullptr) {
    return rects;
  }
  if (!wlOutput->hasUsableGeometry()) {
    return rects;
  }
  const std::int32_t outputW = wlOutput->effectiveLogicalWidth();
  const std::int32_t outputH = wlOutput->effectiveLogicalHeight();

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (!instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    const auto* surface = instance->surface.get();
    const std::uint32_t anchor = surface->anchor();
    const bool aTop = (anchor & LayerShellAnchor::Top) != 0;
    const bool aBottom = (anchor & LayerShellAnchor::Bottom) != 0;
    const bool aLeft = (anchor & LayerShellAnchor::Left) != 0;
    const bool aRight = (anchor & LayerShellAnchor::Right) != 0;
    const std::int32_t mTop = surface->marginTop();
    const std::int32_t mRight = surface->marginRight();
    const std::int32_t mBottom = surface->marginBottom();
    const std::int32_t mLeft = surface->marginLeft();
    // surface->width()/height() may be 0 before configure; fall back to BarConfig
    // thickness so we still publish a sensible exclusion for fresh surfaces.
    const auto surfW = static_cast<std::int32_t>(surface->width());
    const auto surfH = static_cast<std::int32_t>(surface->height());

    std::int32_t rectW = surfW;
    std::int32_t rectH = surfH;
    std::int32_t rectX = 0;
    std::int32_t rectY = 0;

    if (aLeft && aRight) {
      rectW = std::max(0, outputW - mLeft - mRight);
      rectX = mLeft;
    } else if (aRight) {
      rectX = std::max(0, outputW - mRight - rectW);
    } else {
      rectX = mLeft;
    }

    if (aTop && aBottom) {
      rectH = std::max(0, outputH - mTop - mBottom);
      rectY = mTop;
    } else if (aBottom) {
      rectY = std::max(0, outputH - mBottom - rectH);
    } else {
      rectY = mTop;
    }

    if (rectW > 0 && rectH > 0) {
      rects.push_back(InputRect{rectX, rectY, rectW, rectH});
    }
  }

  return rects;
}

std::vector<wl_surface*> Bar::allBarSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance != nullptr && instance->surface != nullptr && instanceAcceptsPointerInput(*instance)) {
      if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
        surfaces.push_back(s);
      }
    }
  }
  return surfaces;
}

std::vector<wl_surface*> Bar::caffeineAnchorSurfaces() const {
  std::vector<wl_surface*> surfaces;
  surfaces.reserve(m_instances.size());
  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->surface == nullptr || !instanceAcceptsPointerInput(*instance)) {
      continue;
    }
    if (!barContentVisuallyShown(*instance)) {
      continue;
    }
    if (wl_surface* s = instance->surface->wlSurface(); s != nullptr) {
      surfaces.push_back(s);
    }
  }
  return surfaces;
}

bool Bar::canAttachPanelToBar(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || instance->surface == nullptr || !instance->barConfig.enabled) {
    return false;
  }
  return barSupportsSlideBehavior(instance->barConfig) || instanceEffectivelyVisible(*instance);
}

std::optional<std::string> Bar::layerForBar(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || instance->surface == nullptr || !instance->barConfig.enabled) {
    return std::nullopt;
  }
  return instance->barConfig.layer;
}

LayerShellLayer Bar::highestLayerForOutput(wl_output* output) const noexcept {
  LayerShellLayer highest = LayerShellLayer::Top;
  for (const auto& instance : m_instances) {
    if (instance->output != output || !instance->barConfig.enabled) {
      continue;
    }
    const LayerShellLayer layer = layerShellLayerFromConfig(instance->barConfig.layer);
    if (static_cast<std::uint32_t>(layer) > static_cast<std::uint32_t>(highest)) {
      highest = layer;
    }
  }
  return highest;
}

bool Bar::isAttachedPanelBarSettled(wl_output* output, std::string_view barName) const noexcept {
  const BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr || !barSupportsSlideBehavior(instance->barConfig)) {
    return true;
  }
  constexpr float kSettledThreshold = 0.999F;
  return instance->hideOpacity >= kSettledThreshold;
}

void Bar::revealAutoHideForAttachedPanel(wl_output* output, std::string_view barName) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance != nullptr) {
    revealAutoHideBar(*instance);
  }
}

std::optional<AttachedPanelSource> Bar::attachedSourceGeometry(
    wl_output* output, std::string_view barName, const AttachedPanelSource& requested) const {
  if (m_platform == nullptr) return std::nullopt;
  const auto* instance = instanceForBar(output, barName);
  const auto* info = m_platform->findOutputByWl(output);
  if (instance == nullptr || instance->contentClip == nullptr || info == nullptr
      || !info->hasUsableGeometry() || !instance->barConfig.sectionBackgrounds) return std::nullopt;
  AttachedPanelSource source;
  const auto ringFor = [](const std::vector<std::unique_ptr<Widget>>& widgets) -> std::optional<CountdownRingStyle> {
    const Widget* only = nullptr;
    for (const auto& widget : widgets) {
      if (!widget->outerNode() || !widget->outerNode()->visible() || !widget->outerNode()->participatesInLayout()) continue;
      if (only) return std::nullopt;
      only = widget.get();
    }
    auto style = only ? only->usageRingStyle() : std::nullopt;
    if (style) style->radius = -1.0F; // The receiving contour supplies its own radius.
    return style;
  };
  if (!requested.sectionId.empty()) {
    const auto it = std::ranges::find(instance->dynamicSections, requested.sectionId,
                                      [](const DynamicBarSection& candidate) { return candidate.config.id; });
    if (it == instance->dynamicSections.end()) return std::nullopt;
    source = it->compactSource;
    source.usageRing = ringFor(it->widgets);
  } else {
    const auto index = sourceSectionIndex(requested.section);
    if (!index) return std::nullopt;
    source = instance->compactPanelSources[*index];
    const std::array<const std::vector<std::unique_ptr<Widget>>*, 3> groups{
        &instance->startWidgets, &instance->centerWidgets, &instance->endWidgets};
    source.usageRing = ringFor(*groups[*index]);
  }
  if (!source.valid()) return std::nullopt;
  float clipX = 0.0F, clipY = 0.0F;
  Node::absolutePosition(instance->contentClip, clipX, clipY);
  const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*instance, *info);
  source.x += clipX + surfaceX;
  source.y += clipY + surfaceY;
  return source;
}

const Node* Bar::attachedPanelSourceContent(wl_output* output,std::string_view barName,
                                           const AttachedPanelSource& source) const {
  for (const auto& instance:m_instances) {
    if (instance->output!=output || instance->barConfig.name!=barName) continue;
    if (!source.sectionId.empty()) {
      const auto it = std::ranges::find(instance->dynamicSections, source.sectionId,
                                        [](const DynamicBarSection& candidate) { return candidate.config.id; });
      return it == instance->dynamicSections.end() ? nullptr : it->content;
    }
    const auto index=sourceSectionIndex(source.section);
    if (!index) return nullptr;
    const std::array<const Node*,3> sections{instance->startSection,instance->centerSection,instance->endSection};
    return sections[*index];
  }
  return nullptr;
}

void Bar::setAttachedPanelGeometry(
    wl_output* output, std::string_view barName, std::optional<AttachedPanelGeometry> geometry
) {
  BarInstance* instance = instanceForBar(output, barName);
  if (instance == nullptr) {
    return;
  }

  if (geometry.has_value() && usesStableIslandMorphSurface(instance->barConfig) && m_platform != nullptr) {
    if (const auto* outputInfo = m_platform->findOutputByWl(output);
        outputInfo != nullptr && outputInfo->hasUsableGeometry()) {
      const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*instance, *outputInfo);
      geometry->x = geometry->outputX - surfaceX;
      geometry->y = geometry->outputY - surfaceY;
      geometry->width = geometry->outputWidth;
      geometry->height = geometry->outputHeight;
      geometry->finalOutputX -= surfaceX;
      geometry->finalOutputY -= surfaceY;
    }
  }
  instance->attachedPanelGeometry = geometry;
  syncBarSurfaceChrome(*instance);
  if (instance->surface != nullptr && instance->surface->width() > 0 && instance->surface->height() > 0) {
    applyBarShadowStyle(
        *instance, m_config->config().shell.shadow, static_cast<float>(instance->surface->width()),
        static_cast<float>(instance->surface->height())
    );
    instance->surface->requestUpdate();
  }
}

void Bar::beginAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  ++instance->attachedPopupCount;
}

void Bar::endAttachedPopup(wl_surface* surface) {
  auto* instance = instanceForSurface(surface);
  if (instance == nullptr) {
    return;
  }
  if (instance->attachedPopupCount > 0) {
    --instance->attachedPopupCount;
  }
  if (instance->attachedPopupCount > 0) {
    return;
  }
  instance->pointerInside =
      m_platform != nullptr && m_platform->hasPointerPosition() && m_platform->lastPointerSurface() == surface;
  if (instance->pointerInside) {
    instance->lastPointerSx = static_cast<float>(m_platform->lastPointerX());
    instance->lastPointerSy = static_cast<float>(m_platform->lastPointerY());
    instance->inputDispatcher.pointerEnter(
        instance->lastPointerSx, instance->lastPointerSy, m_platform->lastInputSerial()
    );
  } else {
    instance->inputDispatcher.pointerLeave();
  }
  if (instance->surface != nullptr) {
    instance->surface->requestRedraw();
  }
  if (!instance->pointerInside && m_hoveredInstance == instance) {
    m_hoveredInstance = nullptr;
  } else if (instance->pointerInside) {
    m_hoveredInstance = instance;
  }
  if (!barPointerHideAllowed(*instance) || instance->pointerInside) {
    return;
  }
  const bool suppressAutoHide =
      (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*instance) : false;
  if (!suppressAutoHide) {
    startHideFadeOut(*instance);
  }
}

void Bar::show() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::hide() {
  for (const auto& instance : m_instances) {
    if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
      // bar-hide IPC always frees layout on non-autohide bars (v4 isVisible=false), regardless of reserve_space.
      instance->ipcLayoutReleased = true;
    }
  }
  applyIpcVisibility(false);
}

void Bar::suppressDisplay() {
  if (m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = true;
  m_wasVisibleBeforeOverlaySuppress = isVisible();
  hide();
}

void Bar::unsuppressDisplay() {
  if (!m_overlayDisplaySuppressed) {
    return;
  }
  m_overlayDisplaySuppressed = false;
  if (m_wasVisibleBeforeOverlaySuppress) {
    show();
  }
}

void Bar::toggle() {
  const bool anyEffectivelyVisible = std::ranges::any_of(m_instances, [this](const auto& inst) {
    return inst != nullptr && instanceEffectivelyVisible(*inst);
  });

  if (anyEffectivelyVisible) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && !barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
    }
    applyIpcVisibility(false);
    return;
  }

  for (const auto& instance : m_instances) {
    if (instance != nullptr) {
      instance->ipcLayoutReleased = false;
    }
  }
  applyIpcVisibility(true);
}

void Bar::syncInstances() {
  const auto& outputs = m_platform->outputs();
  const auto& bars = m_config->config().bars;

  const auto geometryChanged = [](const BarInstance& instance, const WaylandOutput& output) {
    return instance.outputLogicalX != output.logicalX
        || instance.outputLogicalY != output.logicalY
        || instance.outputLogicalWidth != output.effectiveLogicalWidth()
        || instance.outputLogicalHeight != output.effectiveLogicalHeight();
  };

  std::erase_if(m_lastActiveWorkspaceByOutput, [&outputs](const auto& pair) {
    return std::ranges::find(outputs, pair.first, &WaylandOutput::name) == outputs.end();
  });

  for (const auto& output : outputs) {
    if (!output.done || !output.hasUsableGeometry()) {
      continue;
    }
    auto& last = m_lastActiveWorkspaceByOutput[output.name];
    if (last.empty()) {
      last = activeWorkspaceId(m_platform->workspaces(output.output));
    }
  }

  // Layer-shell surfaces are output-bound, but some compositors do not reposition an existing surface after that
  // output moves. Recreate every bar on an affected output to preserve their config-order stacking.
  std::unordered_set<std::uint32_t> outputsNeedingRecreate;
  for (const auto& instance : m_instances) {
    const auto output = std::ranges::find(outputs, instance->outputName, &WaylandOutput::name);
    if (output != outputs.end() && output->done && output->hasUsableGeometry() && geometryChanged(*instance, *output)) {
      outputsNeedingRecreate.insert(output->name);
    }
  }

  // Remove instances for outputs that no longer exist or whose output geometry changed.
  std::erase_if(m_instances, [&outputs, &outputsNeedingRecreate, this](const auto& inst) {
    const auto it = std::ranges::find(outputs, inst->outputName, &WaylandOutput::name);
    const bool found = it != outputs.end() && it->done && it->hasUsableGeometry();
    const bool recreate = found && outputsNeedingRecreate.contains(inst->outputName);
    if (!found || recreate) {
      kLog.info("removing instance for output {}", inst->outputName);
    }
    if (!found || recreate) {
      if (inst->surface != nullptr) {
        m_surfaceMap.erase(inst->surface->wlSurface());
      }
      if (m_hoveredInstance == inst.get()) {
        m_hoveredInstance = nullptr;
      }
    }
    return !found || recreate;
  });

  // Create instances for each bar definition × each output
  for (std::size_t barIdx = 0; barIdx < bars.size(); ++barIdx) {
    for (const auto& output : outputs) {
      if (!output.done || !output.hasUsableGeometry()) {
        continue;
      }

      bool exists = std::ranges::any_of(m_instances, [&output, barIdx](const auto& inst) {
        return inst->outputName == output.name && inst->barIndex == barIdx;
      });
      if (!exists) {
        auto resolved = ConfigService::resolveForOutput(bars[barIdx], output);
        if (!resolved.enabled) {
          continue;
        }
        createInstance(output, barIdx, resolved);
      }
    }
  }

  syncIdleInhibitorAnchors();
  reevaluateSmartAutoHide();
}

void Bar::createInstance(const WaylandOutput& output, std::size_t barIndex, const BarConfig& barConfig) {
  auto instance = std::make_unique<BarInstance>();
  instance->outputName = output.name;
  instance->output = output.output;
  instance->scale = output.scale;
  instance->outputLogicalX = output.logicalX;
  instance->outputLogicalY = output.logicalY;
  instance->outputLogicalWidth = output.effectiveLogicalWidth();
  instance->outputLogicalHeight = output.effectiveLogicalHeight();
  instance->barConfig = barConfig;
  instance->barIndex = barIndex;

  const auto anchor = positionToAnchor(barConfig.position);
  const auto surfaceSpec = computeBarSurfaceSpec(barConfig, m_config->config().shell.shadow);

  kLog.info(
      "creating #{} \"{}\" on {} ({}), thickness={} position={} reserve_space={} exclusive_zone={}", barIndex,
      barConfig.name, output.connectorName, output.description, barConfig.thickness, barConfig.position,
      barConfig.reserveSpace, surfaceSpec.exclusiveZone
  );

  auto surfaceConfig = LayerSurfaceConfig{
      .nameSpace = "noctalia-bar-" + barConfig.name,
      .layer = layerShellLayerFromConfig(barConfig.layer),
      .anchor = anchor,
      .width = surfaceSpec.surfaceWidth,
      .height = surfaceSpec.surfaceHeight,
      .exclusiveZone = surfaceSpec.exclusiveZone,
      .marginTop = surfaceSpec.marginTop,
      .marginRight = surfaceSpec.marginRight,
      .marginBottom = surfaceSpec.marginBottom,
      .marginLeft = surfaceSpec.marginLeft,
      .defaultHeight = surfaceSpec.surfaceHeight,
  };

  instance->surface = std::make_unique<LayerSurface>(m_platform->wayland(), std::move(surfaceConfig));
  instance->surface->setRenderContext(m_renderContext);

  auto* inst = instance.get();
  instance->surface->setConfigureCallback([this, inst](std::uint32_t width, std::uint32_t height) {
    buildScene(*inst, width, height);
  });
  instance->surface->setPrepareFrameCallback([this, inst](bool needsUpdate, bool needsLayout) {
    prepareFrame(*inst, needsUpdate, needsLayout);
  });
  instance->surface->setFrameTickCallback([inst](float deltaMs) {
    tickWidgets(inst->startWidgets, deltaMs);
    tickWidgets(inst->centerWidgets, deltaMs);
    tickWidgets(inst->endWidgets, deltaMs);
    for (auto& section : inst->dynamicSections) tickWidgets(section.widgets, deltaMs);
  });

  instance->surface->setAnimationManager(&instance->animations);
  populateWidgets(*instance);

  if (!instance->surface->initialize(output.output)) {
    kLog.warn("failed to initialize surface for output {}", output.name);
    return;
  }

  m_surfaceMap[instance->surface->wlSurface()] = instance.get();
  m_instances.push_back(std::move(instance));
}

void Bar::destroyInstance(std::uint32_t outputName) {
  std::erase_if(m_instances, [outputName, this](const auto& inst) {
    if (inst->surface != nullptr) {
      m_surfaceMap.erase(inst->surface->wlSurface());
    }
    if (m_hoveredInstance == inst.get()) {
      m_hoveredInstance = nullptr;
    }
    return inst->outputName == outputName;
  });
}

void Bar::populateWidgets(BarInstance& instance) {
  const bool retainedSections = instance.dynamicSections.size() == instance.barConfig.sections.size()
      && std::ranges::equal(instance.dynamicSections, instance.barConfig.sections,
                            {}, [](const DynamicBarSection& section) { return section.config.id; },
                            [](const BarSectionConfig& section) { return section.id; });
  if (!retainedSections) instance.dynamicSections.clear();
  if (!instance.barConfig.sections.empty() && !retainedSections) {
    instance.dynamicSections.reserve(instance.barConfig.sections.size());
    for (const auto& config : instance.barConfig.sections) {
      DynamicBarSection section;
      section.config = config;
      section.bindings.resolve(noctalia::bar::WidgetActionBindings::Inputs{
          .widgetActions = &section.config.actionArea.actions,
          .widgetContext = "bar." + instance.barConfig.name + ".section." + section.config.id,
          .widgetName = section.config.id,
          .widgetType = "section",
      });
      instance.dynamicSections.push_back(std::move(section));
    }
  } else if (retainedSections) {
    for (std::size_t i = 0; i < instance.dynamicSections.size(); ++i) {
      auto& section = instance.dynamicSections[i];
      section.config = instance.barConfig.sections[i];
      section.widgets.clear();
      section.capsuleRuns.clear();
      section.bindings.resolve(noctalia::bar::WidgetActionBindings::Inputs{
          .widgetActions = &section.config.actionArea.actions,
          .widgetContext = "bar." + instance.barConfig.name + ".section." + section.config.id,
          .widgetName = section.config.id,
          .widgetType = "section",
      });
    }
  }
  instance.deadZoneBindings.resolve(
      noctalia::bar::WidgetActionBindings::Inputs{
          .widgetDefaults = noctalia::bar::deadZoneGestureDefaults(),
          .widgetActions = &instance.barConfig.deadZone.actions,
          .widgetContext = "bar." + instance.barConfig.name + ".dead_zone",
          .widgetName = instance.barConfig.name,
          .widgetType = "dead zone",
      }
  );
  const std::array<const BarDeadZoneConfig*, 3> lanes{
      &instance.barConfig.startLane, &instance.barConfig.centerLane, &instance.barConfig.endLane};
  const std::array<std::string_view, 3> laneNames{"start_lane", "center_lane", "end_lane"};
  for (std::size_t i = 0; i < lanes.size(); ++i) {
    instance.laneBindings[i].resolve(
        noctalia::bar::WidgetActionBindings::Inputs{
            .widgetActions = &lanes[i]->actions,
            .widgetContext = "bar." + instance.barConfig.name + "." + std::string(laneNames[i]),
            .widgetName = instance.barConfig.name,
            .widgetType = "lane",
        });
  }
  {
    std::string summary;
    for (const auto gesture : noctalia::bar::allGestures()) {
      const auto* action = instance.deadZoneBindings.find(gesture);
      if (action == nullptr) {
        continue;
      }
      if (!summary.empty()) {
        summary += ", ";
      }
      summary += std::format("{}={}", gestureConfigKey(gesture), action->commandLine());
    }
    // Runs once per bar per reload, mirroring the per-widget line: the first thing to check when a
    // dead zone binding does not fire.
    kLog.debug("bar.{}.dead_zone: {}", instance.barConfig.name, summary.empty() ? "no bindings" : summary);
  }

  const auto& widgetConfigs = m_config->config().widgets;
  const auto labelFontWeight = static_cast<FontWeight>(instance.barConfig.fontWeight);
  const std::string barFontFamily = (instance.barConfig.fontFamily && !instance.barConfig.fontFamily->empty())
      ? *instance.barConfig.fontFamily
      : m_config->config().shell.fontFamily;
  // Creates one widget for `name`. When `groupSpec` is set the widget is a member of a capsule group and
  // takes the group's capsule style + foreground; otherwise it resolves its own per-widget/bar capsule.
  std::unordered_map<std::string, std::string> widgetPlacements;
  std::unordered_map<std::string, const BarWidgetPlacementConfig*> widgetPlacementConfigs;
  for (const auto& placement : instance.barConfig.widgetPlacements)
    if (!placement.id.empty() && !placement.widget.empty()) {
      widgetPlacements.insert_or_assign(placement.id, placement.widget);
      widgetPlacementConfigs.insert_or_assign(placement.id, &placement);
    }
  auto createWidget = [&](const std::string& name, std::string_view placementId, std::string_view sectionId,
                          const WidgetBarCapsuleSpec* groupSpec, const std::optional<ColorSpec>* groupForeground,
                          std::vector<std::unique_ptr<Widget>>& dest) {
    const WidgetConfig* wcPtr = nullptr;
    if (auto it = widgetConfigs.find(name); it != widgetConfigs.end()) {
      wcPtr = &it->second;
    }
    const std::string_view widgetType =
        wcPtr != nullptr && !wcPtr->type.empty() ? std::string_view(wcPtr->type) : std::string_view(name);
    const CommonWidgetOptions options =
        resolveCommonWidgetOptions(instance.barConfig, wcPtr, widgetType, instance.barConfig.scale);
    if (!options.enabled) {
      return;
    }
    auto widget = m_widgetFactory->create(
        name, instance.output, options.contentScale, instance.barConfig.position, instance.barConfig.name,
        static_cast<float>(instance.barConfig.widgetSpacing), options.enableScroll
    );
    if (widget == nullptr) {
      return;
    }
    widget->configureRing(options.ring, options.ringSource, options.ringUsageColors, m_sysmon, m_upower, m_fileWatcher);
    widget->setConfigName(name);
    widget->applyCommonOptions(options, labelFontWeight, barFontFamily, std::format("widget.{}", name));
    widget->setActionContext(
        IpcInvocationContext{
            .widgetName = name,
            .widgetType = wcPtr != nullptr ? wcPtr->type : name,
            .barName = instance.barConfig.name,
            .output = instance.output,
        }
    );
    widget->resolveGestureBindings(
        wcPtr != nullptr ? wcPtr->type : name, wcPtr, &instance.barConfig.actions,
        std::format("bar.{}", instance.barConfig.name), &m_actionDispatcher
    );
    auto capsule = groupSpec != nullptr ? *groupSpec : options.capsule;
    if (!placementId.empty()) {
      const std::string widgetSurface = noctalia::bar::barWidgetMaterialTarget(placementId);
      if (m_config->config().shell.materialOverrides.surfaces.contains(widgetSurface)) {
        std::vector<std::string> surfaces{
            "bar", noctalia::bar::barMaterialTarget(instance.barConfig.name)};
        if (!sectionId.empty())
          surfaces.push_back(noctalia::bar::barSectionMaterialTarget(instance.barConfig.name, sectionId));
        surfaces.push_back(widgetSurface);
        widget->setBarMaterialSurfaces(std::move(surfaces));
        if (!capsule.enabled) {
          capsule.enabled = true;
          capsule.padding = 0.0F;
          // The material plane needs an opaque carrier even on a transparent bar;
          // optical tint/opacity remains controlled by the resolved material parameters.
          capsule.opacity = 1.0F;
          capsule.fill = colorSpecFromRole(ColorRole::Surface);
          capsule.border.reset();
        }
      }
    }
    widget->setBarCapsuleSpec(std::move(capsule));
    const BarWidgetPlacementConfig* placement = nullptr;
    if (!placementId.empty()) {
      if (const auto found = widgetPlacementConfigs.find(std::string(placementId)); found != widgetPlacementConfigs.end())
        placement = found->second;
    }
    if (placement != nullptr && placement->foreground.has_value()) {
      widget->setWidgetForeground(placement->foreground);
    } else if (options.color.has_value()) {
      widget->setWidgetForeground(options.color);
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetForeground(*groupForeground);
    } else if (instance.barConfig.widgetColor.has_value()) {
      widget->setWidgetForeground(instance.barConfig.widgetColor);
    }
    if (placement != nullptr && placement->iconForeground.has_value()) {
      widget->setWidgetIconColor(placement->iconForeground);
    } else if (options.iconColor.has_value()) {
      widget->setWidgetIconColor(options.iconColor);
    } else if (groupForeground != nullptr && groupForeground->has_value()) {
      widget->setWidgetIconColor(*groupForeground);
    } else if (instance.barConfig.widgetIconColor.has_value()) {
      widget->setWidgetIconColor(instance.barConfig.widgetIconColor);
    }
    dest.push_back(std::move(widget));
  };

  // Expands a lane's entries: group tokens become contiguous member widgets sharing the group's capsule.
  auto createWidgets = [&](const std::vector<std::string>& names, std::vector<std::unique_ptr<Widget>>& dest,
                           std::string_view sectionId) {
    for (const auto& laneEntry : names) {
      if (isCapsuleGroupToken(laneEntry)) {
        const BarCapsuleGroupStyle* group = findBarCapsuleGroupStyle(instance.barConfig, capsuleGroupTokenId(laneEntry));
        if (group == nullptr) {
          kLog.warn("bar.{}: lane entry \"{}\" has no matching capsule_group", instance.barConfig.name, laneEntry);
          continue;
        }
        if (!group->enabled) {
          continue;
        }
        const WidgetBarCapsuleSpec groupSpec = capsuleSpecFromGroup(instance.barConfig, *group);
        for (const auto& memberEntry : group->members) {
          const auto member = noctalia::bar::resolveBarWidgetLaneEntry(memberEntry, widgetPlacements);
          createWidget(
              std::string(member.widgetConfigName), member.placementId, sectionId, &groupSpec, &group->foreground, dest);
        }
        continue;
      }
      const auto entry = noctalia::bar::resolveBarWidgetLaneEntry(laneEntry, widgetPlacements);
      createWidget(std::string(entry.widgetConfigName), entry.placementId, sectionId, nullptr, nullptr, dest);
    }
  };

  if (instance.dynamicSections.empty()) {
    createWidgets(instance.barConfig.startWidgets, instance.startWidgets, "start");
    createWidgets(instance.barConfig.centerWidgets, instance.centerWidgets, "center");
    createWidgets(instance.barConfig.endWidgets, instance.endWidgets, "end");
  } else {
    for (auto& section : instance.dynamicSections)
      createWidgets(section.config.widgets, section.widgets, section.config.id);
  }

#ifndef NDEBUG
  // Prepend a red "debug" pill to the end section if running a debug build
  auto debugWidget = m_widgetFactory->create(
      "debug_indicator", instance.output, instance.barConfig.scale, instance.barConfig.position,
      instance.barConfig.name, static_cast<float>(instance.barConfig.widgetSpacing)
  );
  if (debugWidget != nullptr) {
    debugWidget->setConfigName("debug_indicator");
    debugWidget->setFontScale(instance.barConfig.fontScale);
    debugWidget->setLabelFontWeight(labelFontWeight);
    debugWidget->setLabelFontFamily(barFontFamily);
    debugWidget->create();
    if (instance.dynamicSections.empty())
      instance.endWidgets.insert(instance.endWidgets.begin(), std::move(debugWidget));
    else
      instance.dynamicSections.back().widgets.insert(
          instance.dynamicSections.back().widgets.begin(), std::move(debugWidget));
  }
#endif
}

void Bar::attachWidgetsToSections(BarInstance& instance) {
  const bool isVertical = instance.barConfig.position == "left" || instance.barConfig.position == "right";
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const bool hoverHighlight = instance.barConfig.hoverHighlight || instance.barConfig.islandHoverGrow > 0.F;

  instance.widgetByRoot.clear();
  instance.hoverHighlightWidget = nullptr;
  if (instance.hoverUnderlay != nullptr) {
    while (!instance.hoverUnderlay->children().empty()) {
      instance.hoverUnderlay->removeChild(instance.hoverUnderlay->children().back().get());
    }
  }

  // Hover overlay: sits above the capsule fill (same zIndex, later sibling) and below the
  // content; fill/visibility are driven by the hover animation.
  auto addHoverBox = [hoverHighlight](Widget& widget, Node& shell) -> Box* {
    if (!hoverHighlight || !widget.wantsBarHoverHighlight()) {
      return nullptr;
    }
    Box* boxPtr = nullptr;
    shell.addChild(
        ui::box({
            .out = &boxPtr,
            .fill = scaleAlpha(widget.widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)), 0.0F),
            .visible = false,
            .configure = [](Box& box) { box.setZIndex(-1); },
        })
    );
    widget.setBarHoverBox(boxPtr);
    return boxPtr;
  };

  auto attach = [&](std::vector<std::unique_ptr<Widget>>& widgets, std::vector<BarCapsuleRun>& capsuleRuns,
                    Flex* section, AttachedPanelSourceSection sourceSection, std::string sourceSectionId = {}) {
    if (section == nullptr) {
      return;
    }

    for (auto& widget : widgets) {
      widget->setAnimationManager(&instance.animations);
      widget->setUpdateCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestUpdate();
        }
      });
      widget->setRedrawCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestRedraw();
        }
      });
      widget->setFrameTickRequestCallback([surface = instance.surface.get()]() {
        if (surface != nullptr) {
          surface->requestFrameTick();
        }
      });
      if (auto* plugin = dynamic_cast<PluginWidget*>(widget.get()); plugin != nullptr) {
        plugin->setUpdateDeferralCallback([]() {
          auto* panel = PanelManager::current();
          return panel != nullptr && panel->isPanelTransitionActive();
        });
      }
      widget->setPanelToggleCallback([this, inst = &instance, sourceSection, sourceSectionId](
                                         std::string_view panelId, std::string_view context,
                                         std::optional<float> anchorSurfaceX, std::optional<float> anchorSurfaceY,
                                         Widget::PanelActivation activation
                                     ) {
        float anchorX = inst->lastPointerSx;
        float anchorY = inst->lastPointerSy;
        if (anchorSurfaceX.has_value()) {
          anchorX = *anchorSurfaceX;
        }
        if (anchorSurfaceY.has_value()) {
          anchorY = *anchorSurfaceY;
        }
        if (m_platform != nullptr && inst->output != nullptr) {
          if (const auto* out = m_platform->findOutputByWl(inst->output); out != nullptr && out->hasUsableGeometry()) {
            const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*inst, *out);
            anchorX += surfaceX;
            anchorY += surfaceY;
          }
        }
        AttachedPanelSource source{.section = sourceSection, .sectionId = sourceSectionId};
        if (m_platform != nullptr && inst->output != nullptr && inst->barConfig.sectionBackgrounds) {
          const auto index = static_cast<std::size_t>(sourceSection) - 1U;
          if (index < inst->sectionBackgrounds.size()) {
            Box* background = inst->sectionBackgrounds[index];
            const auto* output = m_platform->findOutputByWl(inst->output);
            if (background != nullptr && background->visible() && output != nullptr && output->hasUsableGeometry()) {
              float localX = 0.0F;
              float localY = 0.0F;
              Node::absolutePosition(background, localX, localY);
              const auto [surfaceX, surfaceY] = surfaceOriginForOutputLocal(*inst, *output);
              source.x = surfaceX + localX;
              source.y = surfaceY + localY;
              source.width = background->width();
              source.height = background->height();
              source.radii = background->style().radius;
            }
          }
        }
        if (const auto compact = attachedSourceGeometry(inst->output, inst->barConfig.name, source))
          source = *compact;
        PanelOpenRequest request{
            .output = inst->output,
            .anchorX = anchorX,
            .anchorY = anchorY,
            .hasExplicitAnchor = anchorSurfaceX.has_value() || anchorSurfaceY.has_value(),
            .hasAnchorPosition = true,
            .context = context,
            .sourceBarName = inst->barConfig.name,
            .source = source,
        };
        if (activation == Widget::PanelActivation::Open) {
          PanelManager::instance().openPanel(std::string(panelId), request);
        } else {
          PanelManager::instance().togglePanel(std::string(panelId), request);
        }
        if (PanelManager::instance().isOpenPanel(panelId)) {
          suppressTooltipForOpenPanel(panelId);
        }
      });
      if (auto* tray = dynamic_cast<TrayWidget*>(widget.get())) {
        tray->setHoverOverlayParent(instance.hoverUnderlay);
      }

      widget->create();
      if (widget->outerNode() != nullptr) {
        instance.widgetByRoot[widget->outerNode()] = widget.get();
      }
    }

    capsuleRuns.clear();

    auto addPlainWidget = [&](Widget& widget) {
      widget.setBarCapsuleScene(nullptr, nullptr);
      // No capsule: the hover pill lives on the bar-level underlay (unclipped, no layout
      // footprint) and is positioned after sections are laid out.
      if (hoverHighlight
          && instance.hoverUnderlay != nullptr
          && !widget.isBarClickThrough()
          && !widget.noGapAroundMe()) {
        addHoverBox(widget, *instance.hoverUnderlay);
      }
      auto* added = section->addChild(widget.releaseRoot());
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    auto addSingleCapsule = [&](Widget& widget) {
      const auto& cap = widget.barCapsuleSpec();
      auto shell = ui::node({});
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget.contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = scaleAlpha(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            // The material receiver extends beyond the capsule body. Bypass
            // only this shell's rectangular paint clip; content, accordion
            // clipping, ancestor clips and input bounds remain unchanged.
            bg.setBypassParentPaintClip(true);
            bg.setSurfaceRelief(0.7F);
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, Style::borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));
      if (widget.hasBarMaterialSurface())
        bgPtr->setMaterialIdentityPath("surface", "bar-widget", widget.barMaterialSurfaces());
      Box* hoverPtr = addHoverBox(widget, *shellPtr);
      shellPtr->addChild(widget.releaseRoot());
      widget.setBarCapsuleScene(shellPtr, bgPtr);
      if (auto* area = dynamic_cast<InputArea*>(widget.root())) {
        area->setTooltipAnchorNode(shellPtr);
      }
      capsuleRuns.push_back(
          BarCapsuleRun{
              .shell = shellPtr,
              .bg = bgPtr,
              .container = nullptr,
              .content = widget.outerNode(),
              .spec = cap,
              .contentScale = widget.contentScale(),
              .widgets = {&widget},
              .hoverBoxes = hoverPtr != nullptr ? std::vector<Box*>{hoverPtr} : std::vector<Box*>{},
          }
      );
      auto* added = section->addChild(std::move(shell));
      if (widget.noGapAroundMe()) {
        section->setChildGapExcluded(added, true);
      }
    };

    // Members of the same group share one resolved style by construction (see resolveWidgetBarCapsuleSpec),
    // so adjacency + matching group ID is sufficient to merge. Per-widget scale does not split the group:
    // the run is sized from its largest member's scale below so a differently scaled member still fits.
    auto canJoinCapsuleGroup = [](const Widget& first, const Widget& next) {
      const auto& firstSpec = first.barCapsuleSpec();
      const auto& nextSpec = next.barCapsuleSpec();
      return firstSpec.enabled
          && nextSpec.enabled
          && !first.hasBarMaterialSurface()
          && !next.hasBarMaterialSurface()
          && !first.isAnchor()
          && !next.isAnchor()
          && !firstSpec.group.empty()
          && firstSpec.group == nextSpec.group;
    };

    std::size_t index = 0;
    while (index < widgets.size()) {
      auto& widget = widgets[index];
      if (widget->root() == nullptr) {
        ++index;
        continue;
      }

      const auto& cap = widget->barCapsuleSpec();
      if (!cap.enabled) {
        addPlainWidget(*widget);
        ++index;
        continue;
      }

      if (widget->isAnchor() || cap.group.empty()) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      std::size_t runEnd = index + 1;
      while (runEnd < widgets.size()
             && widgets[runEnd] != nullptr
             && widgets[runEnd]->root() != nullptr
             && canJoinCapsuleGroup(*widget, *widgets[runEnd])) {
        ++runEnd;
      }

      if (runEnd - index < 2) {
        addSingleCapsule(*widget);
        ++index;
        continue;
      }

      auto shell = ui::node({});
      Node* shellPtr = shell.get();
      shellPtr->setClipChildren(true);
      const float scale = widget->contentScale();
      Box* bgPtr = nullptr;
      auto capsuleBg = ui::box({
          .out = &bgPtr,
          .fill = scaleAlpha(cap.fill, cap.opacity),
          .configure = [&cap, scale](Box& bg) {
            // The material receiver extends beyond the capsule body. Bypass
            // only this shell's rectangular paint clip; content, accordion
            // clipping, ancestor clips and input bounds remain unchanged.
            bg.setBypassParentPaintClip(true);
            bg.setSurfaceRelief(0.7F);
            if (cap.border.has_value()) {
              bg.setBorder(*cap.border, Style::borderWidth * scale);
            } else {
              bg.clearBorder();
            }
            bg.setZIndex(-1);
          },
      });
      shellPtr->addChild(std::move(capsuleBg));

      // Visual order: with accordion + Start direction the always-visible first member sits last,
      // so the hidden members unfold before it along the lane's main axis. Flex children,
      // run.widgets and run.hoverBoxes must all follow this same order.
      const bool accordion = cap.accordion && (runEnd - index >= 2);
      const bool accordionStart = accordion && cap.accordionDirection == BarAccordionDirection::Start;
      std::vector<std::size_t> memberOrder;
      memberOrder.reserve(runEnd - index);
      if (accordionStart) {
        for (std::size_t memberIndex = index + 1; memberIndex < runEnd; ++memberIndex) {
          memberOrder.push_back(memberIndex);
        }
        memberOrder.push_back(index);
      } else {
        for (std::size_t memberIndex = index; memberIndex < runEnd; ++memberIndex) {
          memberOrder.push_back(memberIndex);
        }
      }

      std::vector<Box*> hoverBoxes;
      if (hoverHighlight) {
        hoverBoxes.reserve(memberOrder.size());
        for (const std::size_t memberIndex : memberOrder) {
          hoverBoxes.push_back(
              instance.hoverUnderlay != nullptr ? addHoverBox(*widgets[memberIndex], *instance.hoverUnderlay) : nullptr
          );
        }
      }

      auto inner = ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .gap = cap.widgetSpacing.value_or(widgetSpacing),
          }
      );
      Flex* innerPtr = inner.get();
      // Accordion members are clipped to the reveal window, not the padded shell: the capsule
      // padding is wider than the member gap at larger padding values, which would otherwise leave
      // the next member's leading edge showing inside the collapsed pill.
      Node* clipPtr = nullptr;
      if (accordion) {
        auto clip = ui::node({});
        clipPtr = clip.get();
        clipPtr->setClipChildren(true);
        clipPtr->addChild(std::move(inner));
        shellPtr->addChild(std::move(clip));
      } else {
        shellPtr->addChild(std::move(inner));
      }

      BarCapsuleRun run;
      run.shell = shellPtr;
      run.bg = bgPtr;
      run.container = innerPtr;
      run.content = innerPtr;
      run.spec = cap;
      run.contentScale = widget->contentScale();
      run.hoverBoxes = std::move(hoverBoxes);
      run.accordion = accordion;
      run.accordionClip = clipPtr;
      run.accordionDirection = cap.accordionDirection;
      run.accordionVisibleIndex = accordionStart ? memberOrder.size() - 1 : 0;

      for (const std::size_t memberIndex : memberOrder) {
        auto& member = widgets[memberIndex];
        member->setBarCapsuleScene(shellPtr, bgPtr);
        run.widgets.push_back(member.get());
        auto* added = innerPtr->addChild(member->releaseRoot());
        if (auto* area = dynamic_cast<InputArea*>(member->root())) {
          area->setTooltipAnchorNode(shellPtr);
        }
        if (member->noGapAroundMe()) {
          innerPtr->setChildGapExcluded(added, true);
        }
      }

      capsuleRuns.push_back(std::move(run));
      section->addChild(std::move(shell));
      index = runEnd;
    }
  };

  attach(instance.startWidgets, instance.startCapsuleRuns, instance.startSection, AttachedPanelSourceSection::Start);
  attach(instance.centerWidgets, instance.centerCapsuleRuns, instance.centerSection, AttachedPanelSourceSection::Center);
  attach(instance.endWidgets, instance.endCapsuleRuns, instance.endSection, AttachedPanelSourceSection::End);
  for (auto& section : instance.dynamicSections) {
    const auto sourceSection = section.config.anchor == BarCenterAlignment::Start
        ? AttachedPanelSourceSection::Start
        : section.config.anchor == BarCenterAlignment::End
        ? AttachedPanelSourceSection::End
        : AttachedPanelSourceSection::Center;
    attach(section.widgets, section.capsuleRuns, section.content, sourceSection, section.config.id);
  }
}

namespace {

  // Walks up from the hovered area to the bar widget that owns it, if any.
  Widget* widgetFromHoveredArea(BarInstance& instance, InputArea* hoveredArea) {
    for (const Node* node = hoveredArea; node != nullptr; node = node->parent()) {
      if (auto it = instance.widgetByRoot.find(node); it != instance.widgetByRoot.end()) {
        return it->second;
      }
    }
    return nullptr;
  }

} // namespace

void Bar::updateWidgetHoverHighlight(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = widgetFromHoveredArea(instance, hoveredArea);
  if (target != nullptr && target->barHoverBox() == nullptr) {
    target = nullptr;
  }
  if (target == instance.hoverHighlightWidget) {
    return;
  }
  if (instance.hoverHighlightWidget != nullptr) {
    animateWidgetHoverHighlight(instance, *instance.hoverHighlightWidget, false);
  }
  instance.hoverHighlightWidget = target;
  if (target != nullptr) {
    animateWidgetHoverHighlight(instance, *target, true);
  }
}

void Bar::animateWidgetHoverHighlight(BarInstance& instance, Widget& widget, bool hovered) {
  Box* box = widget.barHoverBox();
  if (box == nullptr) {
    return;
  }
  const ColorSpec fill = widget.widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface));
  instance.animations.cancelForOwner(box);
  instance.animations.animate(
      widget.barHoverProgress(), hovered ? 1.0F : 0.0F, Style::animFast, Easing::EaseOutCubic,
      [&widget, box, fill, &instance](float progress) {
        widget.setBarHoverProgress(progress);
        box->setVisible(progress > 0.001F);
        const bool geometryHover = instance.barConfig.sectionBackgrounds && instance.barConfig.islandHoverGrow > 0.F;
        box->setFill(scaleAlpha(fill, geometryHover ? 0.F : kWidgetHoverFillAlpha * progress));
        if (geometryHover && instance.surface) instance.surface->requestUpdate();
      },
      {}, box
  );
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::updateAccordionExpansion(BarInstance& instance, InputArea* hoveredArea) {
  Widget* target = widgetFromHoveredArea(instance, hoveredArea);

  auto isPointerInsideRunShell = [&instance](const BarCapsuleRun& run) -> bool {
    return instance.pointerInside && pointInsideNode(run.shell, instance.lastPointerSx, instance.lastPointerSy);
  };

  bool changed = false;
  auto updateRuns = [&](std::vector<BarCapsuleRun>& runs) {
    for (auto& run : runs) {
      if (!run.accordion || run.shell == nullptr) {
        continue;
      }
      bool want = target != nullptr && std::ranges::contains(run.widgets, target);
      if (!want && (run.accordionExpanded || run.accordionProgress > 0.0F)) {
        if (isPointerInsideRunShell(run)) {
          want = true;
        }
      }
      if (want == run.accordionExpanded) {
        continue;
      }
      run.accordionExpanded = want;
      changed = true;
      instance.animations.cancelForOwner(run.shell);
      instance.animations.animate(
          run.accordionProgress, want ? 1.0F : 0.0F, Style::animNormal, Easing::EaseOutCubic,
          [&run, surface = instance.surface.get()](float progress) {
            run.accordionProgress = progress;
            if (surface != nullptr) {
              surface->requestLayout();
            }
          },
          {}, run.shell
      );
    }
  };
  updateRuns(instance.startCapsuleRuns);
  updateRuns(instance.centerCapsuleRuns);
  updateRuns(instance.endCapsuleRuns);
  for (auto& section : instance.dynamicSections) updateRuns(section.capsuleRuns);
  // animate() only queues; the first tick needs a frame requested here.
  if (changed && instance.surface != nullptr) {
    instance.surface->requestLayout();
  }
}

void Bar::rebuildInstanceContents(BarInstance& instance, const BarConfig& newConfig) {
  noctalia::profiling::ScopedTimer t(kLog, std::format("bar: rebuild contents [{}]", newConfig.name));

  // Drop any pointer hover/capture state pointing into the widgets we're about
  // to destroy. Hover will be re-acquired on the next pointer motion.
  instance.inputDispatcher.pointerLeave();

  instance.barConfig = newConfig;

  // Detach old widget root nodes from their sections and destroy the widgets.
  // Widgets release their root into the section on creation, so the section
  // owns those nodes — clearing the section frees the scene tree.
  auto clearChildren = [](Node* node) {
    if (node == nullptr) {
      return;
    }
    while (!node->children().empty()) {
      node->removeChild(node->children().back().get());
    }
  };
  clearChildren(instance.startSection);
  clearChildren(instance.centerSection);
  clearChildren(instance.endSection);
  for (auto& section : instance.dynamicSections) clearChildren(section.content);
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  instance.startWidgets.clear();
  instance.centerWidgets.clear();
  instance.endWidgets.clear();
  instance.startCapsuleRuns.clear();
  instance.centerCapsuleRuns.clear();
  instance.endCapsuleRuns.clear();
  for (auto& section : instance.dynamicSections) {
    section.widgets.clear();
    section.capsuleRuns.clear();
    if (section.content != nullptr) section.content->setGap(widgetSpacing);
  }

  // Refresh section-level layout knobs that may have changed (gap; direction
  // doesn't change because position is part of the surface-fields gate).
  if (instance.startSection != nullptr) {
    instance.startSection->setGap(widgetSpacing);
  }
  if (instance.centerSection != nullptr) {
    instance.centerSection->setGap(widgetSpacing);
  }
  if (instance.endSection != nullptr) {
    instance.endSection->setGap(widgetSpacing);
  }

  populateWidgets(instance);
  attachWidgetsToSections(instance);

  applyBackgroundPalette(instance);
  syncBarSurfaceChrome(instance);

  if (instance.surface != nullptr) {
    // Re-run buildScene at the current surface size so radii / styling pick
    // up changes. The first-frame branch is skipped because sceneRoot is
    // already in place.
    const auto w = instance.surface->width();
    const auto h = instance.surface->height();
    if (w > 0 && h > 0) {
      buildScene(instance, w, h);
    }
    instance.surface->requestLayout();
  }
}

void Bar::tickWidgets(std::vector<std::unique_ptr<Widget>>& widgets, float deltaMs) { ::tickWidgets(widgets, deltaMs); }

bool Bar::widgetsNeedFrameTick(const std::vector<std::unique_ptr<Widget>>& widgets) {
  return ::widgetsNeedFrameTick(widgets);
}

bool Bar::instanceNeedsFrameTick(const BarInstance& instance) {
  return widgetsNeedFrameTick(instance.startWidgets)
      || widgetsNeedFrameTick(instance.centerWidgets)
      || widgetsNeedFrameTick(instance.endWidgets)
      || std::ranges::any_of(instance.dynamicSections, [](const DynamicBarSection& section) {
           return widgetsNeedFrameTick(section.widgets);
         });
}

void Bar::applyBackgroundPalette(BarInstance& instance) {
  if (instance.bg == nullptr) {
    return;
  }
  auto style = instance.bg->style();
  float opacity = instance.barConfig.backgroundOpacity;
  if (instance.barConfig.materialMode == BarMaterialMode::Solid) opacity = 1.0F;
  if (instance.barConfig.materialMode == BarMaterialMode::Glass) opacity = std::min(opacity, 0.72F);
  if (instance.barConfig.materialMode == BarMaterialMode::Transparent) opacity = 0.0F;
  style.fill = resolveColorSpec(scaleAlpha(instance.barConfig.background, opacity));
  style.border = resolveColorSpec(instance.barConfig.border);
  style.borderWidth = instance.barConfig.borderWidth;
  instance.bg->setStyle(style);
  for (auto* background : instance.sectionBackgrounds) {
    if (background == nullptr) continue;
    auto sectionStyle = background->style();
    sectionStyle.fill = style.fill;
    sectionStyle.border = style.border;
    sectionStyle.borderWidth = style.borderWidth;
    background->setStyle(sectionStyle);
  }
}

void Bar::syncBarAutoHideInputRegion(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  if (!instanceAcceptsPointerInput(instance)) {
    instance.surface->setInputRegion({});
    return;
  }
  if (barConfigUsesSlideSurface(instance.barConfig)) {
    const bool fullSurface = instance.pointerInside
        || instance.attachedPopupCount > 0
        || instance.hideOpacity > 0.5F
        || (instance.barConfig.smartAutoHide && instance.smartAutoHidePinnedVisible);
    if (instance.barConfig.sectionBackgrounds && fullSurface) {
      instance.surface->setInputRegion(barSectionRegions(instance, true));
      return;
    }
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, fullSurface));
    return;
  }
  if (instance.barConfig.sectionBackgrounds) {
    instance.surface->setInputRegion(barSectionRegions(instance, true));
    return;
  }
  instance.surface->setInputRegion(
      {barContentInputRegion(instance.barConfig, m_config->config().shell.shadow, surfW, surfH)}
  );
}

void Bar::revealAutoHideBar(BarInstance& instance) {
  if (instance.autoHideDisablePending) {
    return;
  }
  if (!barSupportsSlideBehavior(instance.barConfig) || instance.surface == nullptr || instance.slideRoot == nullptr) {
    return;
  }

  instance.ipcLayoutReleased = false;
  instance.animations.cancelForOwner(instance.slideRoot);
  const float current = instance.hideOpacity;
  wl_output* output = instance.output;
  const std::string barName = instance.barConfig.name;
  const auto notifyAttachedPanel = [output, barName]() {
    PanelManager::instance().onAttachedBarRevealSettled(output, barName);
  };

  constexpr float kSettledThreshold = 0.999F;
  if (current >= kSettledThreshold) {
    const int surfW = static_cast<int>(instance.surface->width());
    const int surfH = static_cast<int>(instance.surface->height());
    instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, true));
    syncBarSurfaceChrome(instance);
    instance.surface->requestRedraw();
    notifyAttachedPanel();
    return;
  }

  instance.animations.animate(
      current, 1.0F, Style::animNormal, Easing::EaseOutCubic,
      [inst = &instance, this](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      notifyAttachedPanel, instance.slideRoot
  );
  const int surfW = static_cast<int>(instance.surface->width());
  const int surfH = static_cast<int>(instance.surface->height());
  instance.surface->setInputRegion(barAutoHideSurfaceInputRegion(instance.barConfig, surfW, surfH, true));
  syncBarSurfaceChrome(instance);
  instance.surface->requestRedraw();
}

void Bar::syncBarSlideLayerTransform(BarInstance& instance) const {
  if (instance.slideRoot == nullptr) {
    return;
  }
  if (instance.barConfig.autoHide
      || instance.barConfig.smartAutoHide
      || instance.ipcLayoutReleased
      || instance.hideOpacity < 0.999F) {
    const float t = 1.0F - instance.hideOpacity;
    instance.slideRoot->setPosition(instance.slideHiddenDx * t, instance.slideHiddenDy * t);
  } else {
    instance.slideRoot->setPosition(0.0F, 0.0F);
  }
}

void Bar::applyBarCompositorBlur(BarInstance& instance) const {
  if (instance.surface == nullptr) {
    return;
  }
  if (!barContentVisuallyShown(instance)
      || instance.barConfig.materialMode == BarMaterialMode::Solid
      || instance.barConfig.materialMode == BarMaterialMode::Transparent) {
    instance.surface->clearBlurRegion();
    return;
  }

  if (instance.barConfig.sectionBackgrounds) {
    instance.surface->setBlurRegion(barSectionRegions(instance));
    return;
  }

  if (instance.bg == nullptr) {
    return;
  }
  float absX = 0.0F;
  float absY = 0.0F;
  Node::absolutePosition(instance.bg, absX, absY);
  const int px = static_cast<int>(std::lround(absX));
  const int py = static_cast<int>(std::lround(absY));
  const int pw = static_cast<int>(std::lround(std::max(0.0F, instance.bg->width())));
  const int ph = static_cast<int>(std::lround(std::max(0.0F, instance.bg->height())));
  const auto concave = barConcaveShape(instance.barConfig);
  // The bg node is the visual rect; tessellateShape expects the body rect and
  // expands it outward by logicalInset itself. With all-convex corners and no
  // inset it takes its own fast path down to a plain rounded rect.
  const int insetL = static_cast<int>(std::lround(concave.logicalInset.left));
  const int insetT = static_cast<int>(std::lround(concave.logicalInset.top));
  const int insetR = static_cast<int>(std::lround(concave.logicalInset.right));
  const int insetB = static_cast<int>(std::lround(concave.logicalInset.bottom));
  auto blurStrips = Surface::tessellateShape(
      px + insetL, py + insetT, pw - insetL - insetR, ph - insetT - insetB, concave.corners, concave.logicalInset,
      concave.radii, 1, instance.bg->cornerPower()
  );
  appendCapsuleRegions(blurStrips, instance.startCapsuleRuns);
  appendCapsuleRegions(blurStrips, instance.centerCapsuleRuns);
  appendCapsuleRegions(blurStrips, instance.endCapsuleRuns);
  for (const auto& section : instance.dynamicSections) appendCapsuleRegions(blurStrips, section.capsuleRuns);
  instance.surface->setBlurRegion(blurStrips);
}

void Bar::startHideFadeOut(BarInstance& instance) {
  if (instance.autoHideDisablePending || instance.smartAutoHidePinnedVisible) {
    return;
  }
  const float current = instance.hideOpacity;
  instance.animations.animate(
      current, 0.0F, Style::animNormal, Easing::EaseInQuad,
      [this, inst = &instance](float v) {
        inst->hideOpacity = v;
        syncBarSlideLayerTransform(*inst);
        syncBarSurfaceChrome(*inst);
      },
      [inst = &instance, this]() {
        if (inst->surface == nullptr) {
          return;
        }
        syncBarAutoHideInputRegion(*inst);
        syncBarSurfaceChrome(*inst);
        inst->surface->requestRedraw();
      },
      instance.slideRoot
  );
  syncBarSurfaceChrome(instance);
  if (instance.surface != nullptr) {
    instance.surface->requestRedraw();
  }
}

void Bar::buildScene(BarInstance& instance, std::uint32_t width, std::uint32_t height) {
  uiAssertNotRendering("Bar::buildScene");
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }
  Renderer& renderer = instance.surface->renderTarget().renderer();

  const auto w = static_cast<float>(width);
  const auto h = static_cast<float>(height);
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const auto widgetSpacing = static_cast<float>(instance.barConfig.widgetSpacing);
  const auto& shadowConfig = m_config->config().shell.shadow;
  const auto shadowOffset = shadowDirectionOffset(shadowConfig.direction);
  const float shadowSize = shell::surface_shadow::enabled(instance.barConfig.shadow, shadowConfig)
      ? static_cast<float>(shell::surface_shadow::kBlurRadius)
      : 0.0F;
  const auto shadowOffsetX = static_cast<float>(shadowOffset.x);
  const auto shadowOffsetY = static_cast<float>(shadowOffset.y);
  const bool isBottom = instance.barConfig.position == "bottom";
  const bool isRight = instance.barConfig.position == "right";
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto concave = barConcaveShape(instance.barConfig);

  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaX = barVisual.x;
  const float barAreaY = barVisual.y;
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  if (instance.sceneRoot == nullptr) {
    instance.sceneRoot = ui::node({});
    instance.sceneRoot->setMaterialSurface("bar");
    instance.sceneRoot->setAnimationManager(&instance.animations);
    instance.sceneRoot->setSize(w, h);

    auto slide = ui::node({});
    slide->setParticipatesInLayout(false);
    instance.slideRoot = instance.sceneRoot->addChild(std::move(slide));

    // Bar background
    instance.bg = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
    instance.bg->setMaterialIdentity("surface", "bar");
    instance.bg->setSurfaceRelief(0.3F);
    for (std::size_t i = 0; i < instance.sectionBackgrounds.size(); ++i) {
      auto* background = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
      background->setMaterialIdentity("surface", "bar");
      background->setSurfaceRelief(0.3F);
      background->setHitTestVisible(false);
      background->setVisible(false);
      instance.sectionBackgrounds[i] = background;
      auto* shadow = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
      shadow->setZIndex(-1);
      shadow->setHitTestVisible(false);
      shadow->setVisible(false);
      instance.sectionShadows[i] = shadow;
    }
    for (auto& section : instance.dynamicSections) {
      section.background = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
      section.background->setMaterialIdentity("surface", "bar", "bar");
      section.background->setSurfaceRelief(0.3F);
      section.background->setHitTestVisible(false);
      section.inputEnvelope = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
      section.inputEnvelope->setFill(clearColorSpec());
      section.inputEnvelope->setHitTestVisible(false);
      section.shadow = static_cast<Box*>(instance.slideRoot->addChild(ui::box()));
      section.shadow->setZIndex(-1);
      section.shadow->setHitTestVisible(false);
      section.shadow->setVisible(false);
    }

    // Shadow — bar shape copy rendered with large SDF softness to simulate a blurred drop shadow.
    if (shadowSize > 0.0F) {
      instance.shadow = static_cast<Box*>(instance.slideRoot->addChild(
          ui::box({
              .configure = [](Box& shadow) { shadow.setHitTestVisible(false); },
          })
      ));

      auto leftClip = ui::node({});
      leftClip->setClipChildren(true);
      leftClip->setZIndex(-1);
      instance.shadowLeftClip = instance.slideRoot->addChild(std::move(leftClip));
      instance.shadowLeft = static_cast<Box*>(instance.shadowLeftClip->addChild(ui::box()));

      auto rightClip = ui::node({});
      rightClip->setClipChildren(true);
      rightClip->setZIndex(-1);
      instance.shadowRightClip = instance.slideRoot->addChild(std::move(rightClip));
      instance.shadowRight = static_cast<Box*>(instance.shadowRightClip->addChild(ui::box()));
    }
    // Note: shadow is inserted before bar sections so it renders below them (z=-1 is set below).

    auto contentClip = ui::node({});
    contentClip->setClipChildren(true);
    instance.contentClip = instance.slideRoot->addChild(std::move(contentClip));

    auto hoverUnderlay = ui::node({});
    hoverUnderlay->setHitTestVisible(false);
    hoverUnderlay->setSize(static_cast<float>(w), static_cast<float>(h));
    instance.hoverUnderlay = instance.slideRoot->addChild(std::move(hoverUnderlay));

    auto makeSlot = [&instance]() {
      auto slot = ui::node({});
      slot->setClipChildren(true);
      return instance.contentClip->addChild(std::move(slot));
    };
    instance.startSlot = makeSlot();
    instance.centerSlot = makeSlot();
    instance.endSlot = makeSlot();

    // Create section boxes
    auto makeSection = [widgetSpacing, isVertical]() {
      auto section = ui::flex(
          isVertical ? FlexDirection::Vertical : FlexDirection::Horizontal,
          {
              .align = FlexAlign::Center,
              .gap = widgetSpacing,
          }
      );
      section->setMirrorInRtl(false);
      return section;
    };

    instance.startSection = static_cast<Flex*>(instance.startSlot->addChild(makeSection()));
    instance.centerSection = static_cast<Flex*>(instance.centerSlot->addChild(makeSection()));
    instance.endSection = static_cast<Flex*>(instance.endSlot->addChild(makeSection()));

    for (auto& section : instance.dynamicSections) {
      auto slot = ui::node({});
      // Explicit-overlap sections must remain paint-visible outside their
      // natural slot; the surface input union still follows the painted box.
      slot->setClipChildren(!section.config.allowOverlap);
      section.slot = instance.contentClip->addChild(std::move(slot));
      section.content = static_cast<Flex*>(section.slot->addChild(makeSection()));
    }

    attachWidgetsToSections(instance);

    // Wire up InputDispatcher for this instance
    instance.inputDispatcher.setSceneRoot(instance.sceneRoot.get());
    instance.inputDispatcher.setCursorShapeCallback([this](std::uint32_t serial, std::uint32_t shape) {
      m_platform->setCursorShape(serial, shape);
    });
    instance.inputDispatcher.setHoverChangeCallback([this, inst = &instance](InputArea* /*old*/, InputArea* next) {
      if (next != nullptr) {
        next->setTooltipPlacement(tooltipPlacementAwayFromEdge(inst->barConfig.position));
      }
      TooltipManager::instance().onBarHoverChange(next, inst->surface->layerSurface(), inst->output);
      updateWidgetHoverHighlight(*inst, next);
      updateAccordionExpansion(*inst, next);
    });

    if (instance.barConfig.smartAutoHide && m_platform != nullptr) {
      instance.smartAutoHidePinnedVisible = smartAutoHideWantsPinnedVisible(*m_platform, instance.output);
    }
    if (barConfigUsesSlideSurface(instance.barConfig)) {
      instance.slideRoot->setOpacity(1.0F);
      const bool startHidden =
          instance.barConfig.smartAutoHide ? !instance.smartAutoHidePinnedVisible : instance.barConfig.autoHide;
      instance.hideOpacity = startHidden ? 0.0F : 1.0F;
    } else {
      instance.slideRoot->setOpacity(1.0F);
      instance.hideOpacity = 1.0F;
    }

    instance.surface->setSceneRoot(instance.sceneRoot.get());
  }

  // Update root size on reconfigure
  instance.sceneRoot->setSize(w, h);
  if (instance.slideRoot != nullptr) {
    instance.slideRoot->setSize(w, h);
  }
  if (instance.hoverUnderlay != nullptr) {
    instance.hoverUnderlay->setSize(w, h);
  }

  // Background covers only the bar visual area (not the shadow extension).
  // Keep it exactly aligned with the shadow shape; the shadow shader now
  // draws only outside the rect, so any size mismatch is visible at corners.
  if (instance.bg != nullptr) {
    const RoundedRectStyle bgStyle{
        .fill = resolveColorSpec(scaleAlpha(instance.barConfig.background, instance.barConfig.backgroundOpacity)),
        .border = resolveColorSpec(instance.barConfig.border),
        .fillMode = FillMode::Solid,
        .corners = concave.corners,
        .logicalInset = concave.logicalInset,
        .radius = concave.radii,
        .softness = 0.0F,
        .borderWidth = instance.barConfig.borderWidth,
    };
    instance.bg->setStyle(bgStyle);
    // (barAreaX/Y/W/H) is the body; the shader expands outward by logicalInset into
    // the visual rect, so the node must be sized to the visual rect.
    instance.bg->setPosition(barAreaX - concave.logicalInset.left, barAreaY - concave.logicalInset.top);
    instance.bg->setSize(
        barAreaW + concave.logicalInset.left + concave.logicalInset.right,
        barAreaH + concave.logicalInset.top + concave.logicalInset.bottom
    );
  }

  instance.paletteConn = paletteChanged().connect([inst = &instance] {
    applyBackgroundPalette(*inst);
    if (inst->surface != nullptr) {
      inst->surface->requestUpdate();
    }
  });
  if (instance.contentClip != nullptr) {
    instance.contentClip->setClipChildren(!(instance.barConfig.sectionBackgrounds && instance.barConfig.islandHoverGrow > 0.F));
    if (usesStableIslandMorphSurface(instance.barConfig)) {
      instance.contentClip->setPosition(isVertical ? barAreaX : 0.0F, isVertical ? 0.0F : barAreaY);
      instance.contentClip->setSize(isVertical ? barAreaW : w, isVertical ? h : barAreaH);
    } else {
      instance.contentClip->setPosition(barAreaX, barAreaY);
      instance.contentClip->setSize(barAreaW, barAreaH);
    }
  }

  applyBarShadowStyle(instance, shadowConfig, w, h);

  const float mainSpan = usesStableIslandMorphSurface(instance.barConfig) ? (isVertical ? h : w)
                                                                         : (isVertical ? barAreaH : barAreaW);
  const float mainInsetStart = usesStableIslandMorphSurface(instance.barConfig) ? (isVertical ? barAreaY : barAreaX)
                                                                                : 0.0F;
  const float mainInsetEnd = usesStableIslandMorphSurface(instance.barConfig)
      ? std::max(0.0F, mainSpan - mainInsetStart - (isVertical ? barAreaH : barAreaW))
      : 0.0F;
  layoutBarSections(
      instance, renderer, barAreaW, barAreaH, padding, isVertical, mainSpan, mainInsetStart, mainInsetEnd
  );
  syncTransientActivityGeometry(instance);

  float contentLeft = barAreaX;
  float contentTop = barAreaY;
  float contentRight = barAreaX + barAreaW;
  float contentBottom = barAreaY + barAreaH;
  if (instance.shadow != nullptr) {
    const float sx = barAreaX + shadowOffsetX;
    const float sy = barAreaY + shadowOffsetY;
    contentLeft = std::min(contentLeft, sx);
    contentTop = std::min(contentTop, sy);
    contentRight = std::max(contentRight, sx + barAreaW);
    contentBottom = std::max(contentBottom, sy + barAreaH);
  }
  // Concave spikes extend past the body on the inner edge; include them so the bar
  // slides fully off-screen when hidden.
  contentLeft -= concave.logicalInset.left;
  contentTop -= concave.logicalInset.top;
  contentRight += concave.logicalInset.right;
  contentBottom += concave.logicalInset.bottom;
  const auto hiddenDelta = computeAutoHideHiddenDelta(
      isVertical, isBottom, isRight, w, h, contentLeft, contentTop, contentRight, contentBottom
  );
  instance.slideHiddenDx = hiddenDelta.first;
  instance.slideHiddenDy = hiddenDelta.second;
  syncBarSlideLayerTransform(instance);

  syncBarAutoHideInputRegion(instance);
  syncBarSurfaceChrome(instance);
}

void Bar::updateWidgets(BarInstance& instance) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }
  Renderer& renderer = instance.surface->renderTarget().renderer();

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  auto updateSection = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
    for (auto& widget : widgets) {
      if (widget->root() == nullptr) {
        continue;
      }
      widget->update(renderer);
      widget->layout(renderer, barAreaW, barAreaH);
    }
  };

  updateSection(instance.startWidgets);
  updateSection(instance.centerWidgets);
  updateSection(instance.endWidgets);
  for (auto& section : instance.dynamicSections) updateSection(section.widgets);
  const float mainSpan = usesStableIslandMorphSurface(instance.barConfig) ? (isVertical ? h : w)
                                                                         : (isVertical ? barAreaH : barAreaW);
  const float mainInsetStart = usesStableIslandMorphSurface(instance.barConfig)
      ? (isVertical ? barVisual.y : barVisual.x) : 0.0F;
  const float mainInsetEnd = usesStableIslandMorphSurface(instance.barConfig)
      ? std::max(0.0F, mainSpan - mainInsetStart - (isVertical ? barAreaH : barAreaW)) : 0.0F;
  layoutBarSections(
      instance, renderer, barAreaW, barAreaH, padding, isVertical, mainSpan, mainInsetStart, mainInsetEnd
  );
  syncTransientActivityGeometry(instance);
  if (instance.barConfig.sectionBackgrounds || !instance.dynamicSections.empty()) syncBarSurfaceChrome(instance);
}

void Bar::syncTransientActivityGeometry(BarInstance& instance) {
  for (auto& section : instance.dynamicSections) {
    if (section.activityRoot == nullptr || section.background == nullptr) continue;
    const float width = section.background->width();
    const float height = section.background->height();
    section.activityRoot->setPosition(section.background->x(), section.background->y());
    section.activityRoot->setSize(width, height);
    if (section.activityBackground != nullptr) section.activityBackground->setSize(width, height);
    if (section.activityContent != nullptr) section.activityContent->setSize(width, height);
  }
}

bool Bar::canPresentTransientActivity(const TransientActivityRoute& route) const {
  if (route.presentation == TransientActivityPresentation::Attached)
    return !transientActivityAnchors(route).empty();
  if (route.presentation != TransientActivityPresentation::Section || route.section.empty()) return false;
  const wl_output* focused = m_platform != nullptr ? m_platform->preferredInteractiveOutput() : nullptr;
  return std::ranges::any_of(m_instances, [&](const auto& owned) {
    if (owned == nullptr || owned->sceneRoot == nullptr || !instanceEffectivelyVisible(*owned)) return false;
    if (!route.bar.empty() && owned->barConfig.name != route.bar) return false;
    if (route.output == "focused" && owned->output != focused) return false;
    if (route.output != "focused" && route.output != "all") {
      const auto out = std::ranges::find(m_platform->outputs(), owned->output, &WaylandOutput::output);
      if (out == m_platform->outputs().end() || !outputMatchesSelector(route.output, *out)) return false;
    }
    const auto section = std::ranges::find(owned->dynamicSections, route.section, [](const DynamicBarSection& value) {
      return value.config.id;
    });
    return section != owned->dynamicSections.end() && section->background != nullptr
        && section->compactSource.width > 0.0F && section->compactSource.height > 0.0F;
  });
}

std::vector<TransientActivityAnchor>
Bar::transientActivityAnchors(const TransientActivityRoute& route) const {
  std::vector<TransientActivityAnchor> anchors;
  if (route.presentation != TransientActivityPresentation::Attached || route.section.empty()
      || m_platform == nullptr) return anchors;
  const wl_output* focused = m_platform->preferredInteractiveOutput();
  for (const auto& owned : m_instances) {
    if (owned == nullptr || owned->surface == nullptr || owned->sceneRoot == nullptr
        || !instanceEffectivelyVisible(*owned)) continue;
    if (!route.bar.empty() && owned->barConfig.name != route.bar) continue;
    if (route.output == "focused" && owned->output != focused) continue;
    if (route.output != "focused" && route.output != "all") {
      const auto out = std::ranges::find(m_platform->outputs(), owned->output, &WaylandOutput::output);
      if (out == m_platform->outputs().end() || !outputMatchesSelector(route.output, *out)) continue;
    }
    const auto section = std::ranges::find(owned->dynamicSections, route.section, [](const DynamicBarSection& value) {
      return value.config.id;
    });
    if (section == owned->dynamicSections.end() || section->background == nullptr
        || section->compactSource.width <= 0.0F
        || section->compactSource.height <= 0.0F || owned->surface->layerSurface() == nullptr) continue;
    anchors.push_back({
        .parent = owned->surface->layerSurface(),
        .output = owned->output,
        .x = section->compactSource.x,
        .y = section->compactSource.y,
        .width = section->compactSource.width,
        .height = section->compactSource.height,
        .barPosition = owned->barConfig.position,
        .inheritedStyle = section->background->style(),
    });
  }
  return anchors;
}

bool Bar::presentTransientActivity(
    const TransientActivityViewModel& activity, const TransientActivityRoute& route
) {
  if (!canPresentTransientActivity(route)) return false;
  withdrawTransientActivityImmediately(0);
  const wl_output* focused = m_platform != nullptr ? m_platform->preferredInteractiveOutput() : nullptr;
  bool mounted = false;
  for (auto& owned : m_instances) {
    if (owned == nullptr || owned->slideRoot == nullptr || !instanceEffectivelyVisible(*owned)) continue;
    if (!route.bar.empty() && owned->barConfig.name != route.bar) continue;
    if (route.output == "focused" && owned->output != focused) continue;
    if (route.output != "focused" && route.output != "all") {
      const auto out = std::ranges::find(m_platform->outputs(), owned->output, &WaylandOutput::output);
      if (out == m_platform->outputs().end() || !outputMatchesSelector(route.output, *out)) continue;
    }
    auto section = std::ranges::find(owned->dynamicSections, route.section, [](const DynamicBarSection& value) {
      return value.config.id;
    });
    if (section == owned->dynamicSections.end() || section->background == nullptr
        || section->compactSource.width <= 0.0F || section->compactSource.height <= 0.0F) continue;

    auto root = ui::inputArea({});
    root->setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_RIGHT}));
    if (activity.hoverChanged) {
      root->setOnEnter([hoverChanged = activity.hoverChanged](const InputArea::PointerData&) {
        hoverChanged(true);
      });
      root->setOnLeave([hoverChanged = activity.hoverChanged]() { hoverChanged(false); });
    }
    if (activity.activate) {
      root->setOnClick([activate = activity.activate](const InputArea::PointerData& data) {
        if (data.button == BTN_LEFT) activate();
      });
    }
    if (activity.dismiss) {
      root->setOnClick([activate = activity.activate, dismiss = activity.dismiss](const InputArea::PointerData& data) {
        if (data.button == BTN_RIGHT) dismiss();
        else if (data.button == BTN_LEFT && activate) activate();
      });
    }
    root->setZIndex(40);
    root->setParticipatesInLayout(false);
    root->setClipChildren(true);

    auto background = ui::box({
        .configure = [material = route.material, inherited = section->background->style()](Box& box) {
          box.setMaterialIdentity("surface", "activity", "bar");
          if (material == TransientActivityMaterial::Transparent) box.setFill(clearColorSpec());
          else if (material == TransientActivityMaterial::Surface) box.setCardStyle();
          else box.setStyle(inherited);
        },
    });
    section->activityBackground = background.get();
    root->addChild(std::move(background));

    auto row = ui::row({
        .align = FlexAlign::Center,
        .gap = Style::spaceSm,
        .paddingH = Style::spaceSm,
    });
    section->activityContent = row.get();
    row->addChild(ui::glyph({
        .out = &section->activityGlyph,
        .glyph = activity.icon.empty() ? "bell" : activity.icon,
        .glyphSize = Style::fontSizeBody,
    }));
    const std::string primary = activity.title.empty() ? activity.value : activity.title;
    row->addChild(ui::label({
        .out = &section->activityTitle,
        .text = primary,
        .fontSize = Style::fontSizeCaption,
        .fontWeight = FontWeight::Medium,
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .flexGrow = 1.0F,
    }));
    if (!activity.title.empty() && !activity.value.empty()) {
      row->addChild(ui::label({
          .out = &section->activityValue,
          .text = activity.value,
          .fontSize = Style::fontSizeCaption,
          .maxLines = 1,
      }));
    }
    if (activity.showProgress && activity.setProgress) {
      auto slider = std::make_unique<Slider>();
      section->activitySlider = slider.get();
      slider->setRange(0.0, 1.0);
      slider->setStep(0.01);
      slider->setValue(std::clamp(activity.progress, 0.0F, 1.0F));
      slider->setControlHeight(Style::controlHeightSm);
      slider->setWheelAdjustEnabled(true);
      slider->setOnValueChanged([setProgress = activity.setProgress](double value) {
        setProgress(static_cast<float>(value));
      });
      slider->setMinWidth(72.0F);
      slider->setMaxWidth(72.0F);
      row->addChild(std::move(slider));
    } else if (activity.showProgress) {
      row->addChild(ui::progressBar({
          .out = &section->activityProgress,
          .progress = std::clamp(activity.progress, 0.0F, 1.0F),
          .width = 48.0F,
          .height = 4.0F,
      }));
    }
    root->addChild(std::move(row));
    section->activityRoot = static_cast<InputArea*>(owned->slideRoot->addChild(std::move(root)));
    section->activitySerial = activity.serial;
    section->activityMotionEnabled = route.motion == TransientActivityMotion::Inherit
        && MotionService::instance().enabled();
    syncTransientActivityGeometry(*owned);
    if (section->activityMotionEnabled) {
      section->activityRoot->setOpacity(0.0F);
      owned->animations.animate(
          0.0F, 1.0F, Style::animFast, Easing::EaseOutCubic,
          [node = section->activityRoot](float value) { node->setOpacity(value); }, {}, section->activityRoot
      );
    }
    owned->surface->requestLayout();
    mounted = true;
  }
  return mounted;
}

void Bar::withdrawTransientActivity(std::uint64_t serial) {
  for (auto& owned : m_instances) {
    if (owned == nullptr || owned->slideRoot == nullptr) continue;
    for (auto& section : owned->dynamicSections) {
      if (section.activityRoot == nullptr || (serial != 0 && section.activitySerial != serial)) continue;
      InputArea* oldRoot = section.activityRoot;
      const bool animate = section.activityMotionEnabled;
      section.activityRoot = nullptr;
      section.activityBackground = nullptr;
      section.activityContent = nullptr;
      section.activityGlyph = nullptr;
      section.activityTitle = nullptr;
      section.activityValue = nullptr;
      section.activityProgress = nullptr;
      section.activitySlider = nullptr;
      section.activityMotionEnabled = false;
      section.activitySerial = 0;
      if (animate) {
        const auto outputName = owned->outputName;
        owned->animations.animate(
            oldRoot->opacity(), 0.0F, Style::animFast, Easing::EaseOutCubic,
            [oldRoot](float value) { oldRoot->setOpacity(value); },
            [this, outputName, oldRoot]() {
              DeferredCall::callLater([this, outputName, oldRoot]() {
                const auto instance = std::ranges::find(m_instances, outputName, [](const auto& value) {
                  return value != nullptr ? value->outputName : 0U;
                });
                if (instance != m_instances.end() && (*instance)->slideRoot != nullptr)
                  (void)(*instance)->slideRoot->removeChild(oldRoot);
              });
            }, oldRoot);
      } else {
        (void)owned->slideRoot->removeChild(oldRoot);
      }
      owned->surface->requestLayout();
    }
  }
}

void Bar::withdrawTransientActivityImmediately(std::uint64_t serial) {
  for (auto& owned : m_instances) {
    if (owned == nullptr) continue;
    for (auto& section : owned->dynamicSections)
      if (section.activityRoot != nullptr && (serial == 0 || section.activitySerial == serial))
        section.activityMotionEnabled = false;
  }
  withdrawTransientActivity(serial);
}

void Bar::notifyAttachedSourceGeometryChanged(const BarInstance& instance) {
  if (!m_attachedSourceGeometryChangedCallback || !instance.attachedPanelGeometry) return;
  const auto& previous = instance.attachedPanelGeometry->source;
  const auto current = attachedSourceGeometry(instance.output, instance.barConfig.name, previous);
  if (!current || (current->x == previous.x && current->y == previous.y
      && current->width == previous.width && current->height == previous.height
      && current->radii == previous.radii && current->contentOffset == previous.contentOffset
      && current->usageRing == previous.usageRing)) return;
  m_attachedSourceGeometryChangedCallback(instance.output, instance.barConfig.name);
}

void Bar::prepareFrame(BarInstance& instance, bool needsUpdate, bool needsLayout) {
  if (m_renderContext == nullptr || instance.surface == nullptr) {
    return;
  }

  m_renderContext->makeCurrent(instance.surface->renderTarget());
  Renderer& renderer = instance.surface->renderTarget().renderer();

  if (needsUpdate) {
    {
      UiPhaseScope updatePhase(UiPhase::Update);
      updateWidgets(instance);
    }
    notifyAttachedSourceGeometryChanged(instance);
    return;
  }

  if (!needsLayout) {
    return;
  }

  const auto w = static_cast<float>(instance.surface->width());
  const auto h = static_cast<float>(instance.surface->height());
  const auto padding = static_cast<float>(instance.barConfig.padding);
  const bool isVertical = (instance.barConfig.position == "left" || instance.barConfig.position == "right");
  const auto& shadowConfig = m_config->config().shell.shadow;
  const float innerSurfaceExtension = barInnerSurfaceExtension(instance.barConfig, shadowConfig, w, h);
  const auto barVisual = computeBarVisualGeometry(instance.barConfig, shadowConfig, w, h, innerSurfaceExtension);
  const float barAreaW = barVisual.width;
  const float barAreaH = barVisual.height;

  {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    for (auto& widget : instance.startWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    for (auto& widget : instance.centerWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    for (auto& widget : instance.endWidgets) {
      widget->layout(renderer, barAreaW, barAreaH);
    }
    for (auto& section : instance.dynamicSections) {
      for (auto& widget : section.widgets) widget->layout(renderer, barAreaW, barAreaH);
    }
    const float mainSpan = usesStableIslandMorphSurface(instance.barConfig) ? (isVertical ? h : w)
                                                                           : (isVertical ? barAreaH : barAreaW);
    const float mainInsetStart = usesStableIslandMorphSurface(instance.barConfig)
        ? (isVertical ? barVisual.y : barVisual.x) : 0.0F;
    const float mainInsetEnd = usesStableIslandMorphSurface(instance.barConfig)
        ? std::max(0.0F, mainSpan - mainInsetStart - (isVertical ? barAreaH : barAreaW)) : 0.0F;
    layoutBarSections(
        instance, renderer, barAreaW, barAreaH, padding, isVertical, mainSpan, mainInsetStart, mainInsetEnd
    );
    syncTransientActivityGeometry(instance);
  }
  if (instance.barConfig.sectionBackgrounds || !instance.dynamicSections.empty()) syncBarSurfaceChrome(instance);
  notifyAttachedSourceGeometryChanged(instance);
}

bool Bar::onPointerEvent(const PointerEvent& event) {
  bool consumed = false;
  BarInstance* targetInstance = nullptr;
  if (event.surface != nullptr) {
    targetInstance = instanceForSurface(event.surface);
  } else {
    targetInstance = m_hoveredInstance;
  }

  auto routeWidgetPopups = [&](BarInstance& instance) {
    auto routeGroup = [&](std::vector<std::unique_ptr<Widget>>& widgets) {
      for (auto& widget : widgets) {
        if (widget != nullptr && widget->onPointerEvent(event)) {
          return true;
        }
      }
      return false;
    };
    return routeGroup(instance.startWidgets) || routeGroup(instance.centerWidgets) || routeGroup(instance.endWidgets);
  };
  if (targetInstance != nullptr) {
    if (!instanceAcceptsPointerInput(*targetInstance)) {
      clearInstancePointerState(*targetInstance);
      return false;
    }
    if (routeWidgetPopups(*targetInstance)) {
      return true;
    }
  } else {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && instanceAcceptsPointerInput(*instance) && routeWidgetPopups(*instance)) {
        return true;
      }
    }
  }

  if (targetInstance != nullptr && targetInstance->attachedPopupCount > 0) {
    switch (event.type) {
    case PointerEvent::Type::Enter:
      m_hoveredInstance = targetInstance;
      targetInstance->pointerInside = true;
      break;
    case PointerEvent::Type::Leave:
      targetInstance->pointerInside = false;
      if (m_hoveredInstance == targetInstance) {
        m_hoveredInstance = nullptr;
      }
      break;
    case PointerEvent::Type::Motion:
    case PointerEvent::Type::Button:
    case PointerEvent::Type::Axis:
      if (event.type == PointerEvent::Type::Button && event.button == BTN_RIGHT && event.pressed) {
        const auto sx = static_cast<float>(event.sx);
        const auto sy = static_cast<float>(event.sy);
        if (!handleBarDeadZoneButton(*targetInstance, sx, sy, event.button, m_platform, m_actionDispatcher)) {
          openPanelAtBarPointer(
              *targetInstance, sx, sy, m_platform, targetInstance->barConfig.name, "control-center", "home", true
          );
        }
        return true;
      }
      break;
    }
    return false;
  }

  switch (event.type) {
  case PointerEvent::Type::Enter: {
    auto it = m_surfaceMap.find(event.surface);
    if (it == m_surfaceMap.end()) {
      break;
    }
    m_hoveredInstance = it->second;
    BarInstance* const entered = m_hoveredInstance;
    entered->lastPointerSx = static_cast<float>(event.sx);
    entered->lastPointerSy = static_cast<float>(event.sy);
    entered->pointerInside = true;
    entered->inputDispatcher.pointerEnter(static_cast<float>(event.sx), static_cast<float>(event.sy), event.serial);
    // pointerEnter can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != entered) {
      break;
    }
    if (barSupportsSlideBehavior(m_hoveredInstance->barConfig) && m_hoveredInstance->sceneRoot != nullptr) {
      revealAutoHideBar(*m_hoveredInstance);
    }
    break;
  }
  case PointerEvent::Type::Leave: {
    if (m_hoveredInstance != nullptr) {
      m_hoveredInstance->pointerInside = false;
      m_hoveredInstance->inputDispatcher.pointerLeave();
      updateAccordionExpansion(*m_hoveredInstance, nullptr);
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(*m_hoveredInstance) : false;
      if (barPointerHideAllowed(*m_hoveredInstance) && !suppressAutoHide) {
        startHideFadeOut(*m_hoveredInstance);
      }
      m_hoveredInstance = nullptr;
    }
    break;
  }
  case PointerEvent::Type::Motion: {
    if (m_hoveredInstance == nullptr)
      break;
    BarInstance* const hovered = m_hoveredInstance;
    hovered->lastPointerSx = static_cast<float>(event.sx);
    hovered->lastPointerSy = static_cast<float>(event.sy);
    hovered->inputDispatcher.pointerMotion(static_cast<float>(event.sx), static_cast<float>(event.sy), 0);
    // pointerMotion can re-enter the Wayland event loop (tooltip popup work),
    // which may clear or change m_hoveredInstance before we dereference it.
    if (m_hoveredInstance != hovered) {
      break;
    }
    updateAccordionExpansion(*hovered, hovered->inputDispatcher.hoveredArea());
    break;
  }
  case PointerEvent::Type::Button: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    bool pressed = event.pressed;
    consumed = m_hoveredInstance->inputDispatcher.pointerButton(
        sx, sy, event.button, pressed, event.serial, event.time, event.touch
    );
    if (pressed && !consumed) {
      if (handleBarDeadZoneButton(*m_hoveredInstance, sx, sy, event.button, m_platform, m_actionDispatcher)) {
        consumed = true;
      }
    }
    break;
  }
  case PointerEvent::Type::Axis: {
    if (m_hoveredInstance == nullptr)
      break;
    m_hoveredInstance->lastPointerSx = static_cast<float>(event.sx);
    m_hoveredInstance->lastPointerSy = static_cast<float>(event.sy);
    const auto sx = static_cast<float>(event.sx);
    const auto sy = static_cast<float>(event.sy);
    const bool axisConsumed = m_hoveredInstance->inputDispatcher.pointerAxis(
        sx, sy, event.axis, event.axisSource, event.axisValue, event.axisDiscrete, event.axisValue120, event.axisLines,
        event.axisGestureSerial
    );
    if (!axisConsumed) {
      handleBarDeadZoneAxis(*m_hoveredInstance, sx, sy, event, m_platform, m_actionDispatcher);
    }
    break;
  }
  }

  // Trigger redraw if any widget changed visual state
  if (m_hoveredInstance != nullptr
      && m_hoveredInstance->sceneRoot != nullptr
      && (m_hoveredInstance->sceneRoot->paintDirty() || m_hoveredInstance->sceneRoot->layoutDirty())) {
    if (m_hoveredInstance->sceneRoot->layoutDirty()) {
      m_hoveredInstance->surface->requestLayout();
    } else {
      m_hoveredInstance->surface->requestRedraw();
    }
  }

  return consumed;
}

BarInstance* Bar::instanceForSurface(wl_surface* surface) const noexcept {
  if (surface == nullptr) {
    return nullptr;
  }
  const auto it = m_surfaceMap.find(surface);
  return it != m_surfaceMap.end() ? it->second : nullptr;
}

BarInstance* Bar::instanceForOutput(wl_output* output) const noexcept { return instanceForBar(output, {}); }

BarInstance* Bar::instanceForBar(wl_output* output, std::string_view barName) const noexcept {
  if (output == nullptr) {
    return nullptr;
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output != output || instance->surface == nullptr) {
      continue;
    }
    if (barName.empty() || instance->barConfig.name == barName) {
      return instance.get();
    }
  }
  return nullptr;
}

std::optional<std::string> Bar::collectBarIpcInstances(
    std::optional<std::string> barName, std::optional<std::string> monitorSelector,
    std::vector<BarInstance*>& instancesOut
) {
  instancesOut.clear();

  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  if (barName.has_value()) {
    const bool knownBar = std::ranges::contains(m_config->config().bars, *barName, &BarConfig::name);
    if (!knownBar) {
      if (!monitorSelector.has_value()) {
        monitorSelector = std::move(barName);
        barName = std::nullopt;
      } else {
        std::vector<std::string> knownBars;
        knownBars.reserve(m_config->config().bars.size());
        for (const auto& bar : m_config->config().bars) {
          knownBars.push_back(bar.name);
        }
        const std::string suffix =
            knownBars.empty() ? std::string() : std::string("; known: ") + StringUtils::join(knownBars, ", ");
        return "error: unknown bar \"" + std::string(*barName) + "\"" + suffix + "\n";
      }
    }
  }

  const auto matchesBar = [&](const BarInstance& instance) {
    return !barName.has_value() || instance.barConfig.name == *barName;
  };

  if (!monitorSelector.has_value()) {
    for (const auto& instance : m_instances) {
      if (instance != nullptr && matchesBar(*instance)) {
        instancesOut.push_back(instance.get());
      }
    }
    if (instancesOut.empty()) {
      if (barName.has_value()) {
        return "error: no instances matched bar \"" + std::string(*barName) + "\"\n";
      }
      return "error: no bar instances are active\n";
    }
    return std::nullopt;
  }

  if (m_platform == nullptr) {
    return "error: bar service not initialized\n";
  }

  const std::string selector(*monitorSelector);
  std::vector<std::string> outputMatches;
  std::vector<std::string> knownOutputs;
  for (const auto& output : m_platform->outputs()) {
    if (output.connectorName.empty()) {
      continue;
    }
    knownOutputs.push_back(output.connectorName);
    if (outputMatchesSelector(selector, output)) {
      outputMatches.push_back(output.connectorName);
    }
  }

  std::ranges::sort(knownOutputs);
  knownOutputs.erase(std::ranges::unique(knownOutputs).begin(), knownOutputs.end());
  std::ranges::sort(outputMatches);
  outputMatches.erase(std::ranges::unique(outputMatches).begin(), outputMatches.end());

  if (outputMatches.empty()) {
    std::string error = "error: unknown monitor selector \"" + selector + "\"";
    if (!knownOutputs.empty()) {
      error += " (available: " + StringUtils::join(knownOutputs, ", ") + ")";
    }
    error += "\n";
    return error;
  }
  if (outputMatches.size() > 1) {
    return "error: monitor selector \""
        + selector
        + "\" matched multiple outputs: "
        + StringUtils::join(outputMatches, ", ")
        + "\n";
  }

  for (const auto& instance : m_instances) {
    if (instance == nullptr || instance->output == nullptr || !matchesBar(*instance)) {
      continue;
    }
    const auto it = std::find_if(
        m_platform->outputs().begin(), m_platform->outputs().end(),
        [&instance](const WaylandOutput& output) { return output.output == instance->output; }
    );
    if (it != m_platform->outputs().end() && it->connectorName == outputMatches.front()) {
      instancesOut.push_back(instance.get());
    }
  }

  if (instancesOut.empty()) {
    std::string error = "error: no instances matched";
    if (barName.has_value()) {
      error += " bar \"" + std::string(*barName) + "\"";
    }
    error += " on \"" + outputMatches.front() + "\"\n";
    return error;
  }

  return std::nullopt;
}

namespace {

  [[nodiscard]] std::optional<std::string> parseBarVisibilityIpcArgs(
      std::string_view command, std::string_view args, std::optional<std::string>& barName,
      std::optional<std::string>& monitorSelector
  ) {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() > 2) {
      return "error: usage: " + std::string(command) + " [bar-name] [monitor-selector]\n";
    }
    barName = std::nullopt;
    monitorSelector = std::nullopt;
    if (!parts.empty() && !parts[0].empty()) {
      barName = parts[0];
    }
    if (parts.size() >= 2 && !parts[1].empty()) {
      monitorSelector = parts[1];
    }
    return std::nullopt;
  }

} // namespace

std::string Bar::showBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-show", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::hideBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-hide", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (!barSupportsSlideBehavior(instance->barConfig)) {
      instance->ipcLayoutReleased = true;
    }
    setInstanceIpcVisible(*instance, false);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const bool anyEffectivelyVisible = std::ranges::any_of(targets, [this](const BarInstance* instance) {
    return instance != nullptr && instanceEffectivelyVisible(*instance);
  });

  if (anyEffectivelyVisible) {
    for (BarInstance* instance : targets) {
      if (!barSupportsSlideBehavior(instance->barConfig)) {
        instance->ipcLayoutReleased = true;
      }
      setInstanceIpcVisible(*instance, false);
      syncBarSurfaceChrome(*instance);
    }
    return "ok\n";
  }

  for (BarInstance* instance : targets) {
    instance->ipcLayoutReleased = false;
    setInstanceIpcVisible(*instance, true);
    syncBarSurfaceChrome(*instance);
  }
  return "ok\n";
}

std::string Bar::toggleBarReserveSpaceIpc(std::string_view args) {
  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (const auto parseError = parseBarVisibilityIpcArgs("bar-reserve-toggle", args, barName, monitorSelector)) {
    return *parseError;
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  for (BarInstance* instance : targets) {
    if (instance != nullptr) {
      instance->barConfig.reserveSpace = !instance->barConfig.reserveSpace;
      syncBarExclusiveZone(*instance);
    }
  }
  return "ok\n";
}

std::string Bar::setBarAutoHideIpc(std::string_view args) {
  if (m_config == nullptr) {
    return "error: config service not initialized\n";
  }

  const auto parts = noctalia::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-auto-hide-set <on|off|smart|true|false|1|0> [bar-name] [monitor-selector]\n";
  }

  const std::string& value = parts[0];
  bool enabled = false;
  bool smart = false;
  if (value == "on" || value == "true" || value == "1") {
    enabled = true;
  } else if (value == "off" || value == "false" || value == "0") {
    enabled = false;
  } else if (value == "smart") {
    smart = true;
  } else {
    return "error: invalid value (use on/off/smart, true/false, 1/0)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  auto applyTransientAutoHide = [this, enabled, smart](BarInstance& instance) {
    auto applySurfaceSpec = [this](BarInstance& inst) {
      if (inst.surface == nullptr) {
        return;
      }
      const auto spec = computeBarSurfaceSpec(inst.barConfig, m_config->config().shell.shadow);
      inst.surface->setMargins(spec.marginTop, spec.marginRight, spec.marginBottom, spec.marginLeft);
      inst.surface->requestSize(spec.surfaceWidth, spec.surfaceHeight);
    };

    instance.ipcLayoutReleased = false;
    instance.autoHideDisablePending = false;
    instance.animations.cancelForOwner(instance.slideRoot);

    instance.barConfig.autoHide = enabled;
    instance.barConfig.smartAutoHide = smart;

    if (enabled || smart) {
      applySurfaceSpec(instance);
      if (instance.slideRoot != nullptr) {
        instance.slideRoot->setOpacity(1.0F);
      }
      const bool suppressAutoHide =
          (m_autoHideSuppressionCallback != nullptr) ? m_autoHideSuppressionCallback(instance) : false;
      if (instance.pointerInside || instance.attachedPopupCount > 0 || suppressAutoHide) {
        revealAutoHideBar(instance);
      } else {
        startHideFadeOut(instance);
      }
      return;
    }

    if (instance.barConfig.autoHide && instance.hideOpacity < 0.999F) {
      const float current = instance.hideOpacity;
      instance.autoHideDisablePending = true;
      instance.animations.animate(
          current, 1.0F, Style::animNormal, Easing::EaseOutCubic,
          [this, inst = &instance](float v) {
            inst->hideOpacity = v;
            syncBarSlideLayerTransform(*inst);
            syncBarSurfaceChrome(*inst);
          },
          [this, inst = &instance, applySurfaceSpec]() {
            inst->autoHideDisablePending = false;
            inst->barConfig.autoHide = false;
            applySurfaceSpec(*inst);
            syncBarSlideLayerTransform(*inst);
            syncBarAutoHideInputRegion(*inst);
            syncBarSurfaceChrome(*inst);
            if (inst->surface != nullptr) {
              inst->surface->requestRedraw();
            }
          },
          instance.slideRoot
      );
      if (instance.surface != nullptr) {
        instance.surface->requestRedraw();
      }
      return;
    }

    instance.barConfig.autoHide = false;
    instance.autoHideDisablePending = false;
    instance.hideOpacity = 1.0F;
    if (instance.slideRoot != nullptr) {
      instance.slideRoot->setOpacity(1.0F);
    }
    applySurfaceSpec(instance);
    syncBarSlideLayerTransform(instance);
    syncBarAutoHideInputRegion(instance);
    syncBarSurfaceChrome(instance);
    if (instance.surface != nullptr) {
      instance.surface->requestRedraw();
    }
  };

  for (BarInstance* instance : targets) {
    applyTransientAutoHide(*instance);
  }
  return "ok\n";
}

std::string Bar::setBarLayerIpc(std::string_view args) {
  const auto parts = noctalia::ipc::splitWords(args);
  if (parts.empty() || parts.size() > 3) {
    return "error: usage: bar-layer-set <top|overlay> [bar-name] [monitor-selector]\n";
  }

  const std::string& layer = parts[0];
  if (layer != "top" && layer != "overlay") {
    return "error: invalid layer (use top or overlay)\n";
  }

  std::optional<std::string> barName;
  std::optional<std::string> monitorSelector;
  if (parts.size() >= 2 && !parts[1].empty()) {
    barName = parts[1];
  }
  if (parts.size() >= 3 && !parts[2].empty()) {
    monitorSelector = parts[2];
  }

  std::vector<BarInstance*> targets;
  if (const auto collectError = collectBarIpcInstances(barName, monitorSelector, targets)) {
    return *collectError;
  }

  const LayerShellLayer shellLayer = layerShellLayerFromConfig(layer);
  for (BarInstance* instance : targets) {
    if (instance == nullptr || instance->surface == nullptr) {
      continue;
    }
    instance->surface->setLayer(shellLayer);
    instance->barConfig.layer = layer;
  }

  return "ok\n";
}

TaskbarWidget* Bar::findTaskbarWidget(const IpcInvocationContext& context) const {
  const auto findIn = [&context](const std::vector<std::unique_ptr<Widget>>& widgets) -> TaskbarWidget* {
    for (const auto& widget : widgets) {
      if (widget->configName() != context.widgetName) {
        continue;
      }
      if (auto* taskbar = dynamic_cast<TaskbarWidget*>(widget.get()); taskbar != nullptr) {
        return taskbar;
      }
    }
    return nullptr;
  };

  for (const auto& instance : m_instances) {
    if (instance->barConfig.name != context.barName || instance->output != context.output) {
      continue;
    }
    for (const auto* section : {&instance->startWidgets, &instance->centerWidgets, &instance->endWidgets}) {
      if (auto* taskbar = findIn(*section); taskbar != nullptr) {
        return taskbar;
      }
    }
  }
  return nullptr;
}

void Bar::registerIpc(IpcService& ipc) {
  // Widget gesture actions dispatch through the same registry.
  m_actionDispatcher.setIpcService(&ipc);

  ipc.bindCycle(noctalia::cli::msg::taskbarCycle, [this, &ipc](const std::string& args) -> std::string {
    const auto parts = noctalia::ipc::splitWords(args);
    if (parts.size() != 1 || (parts[0] != "next" && parts[0] != "prev")) {
      return "error: taskbar-cycle requires <next|prev>\n";
    }
    // Order comes from the taskbar's own model (pins, grouping, per-monitor filter), so the
    // target is a widget instance rather than a global window list.
    const auto& context = ipc.invocationContext();
    if (!context.has_value() || context->widgetName.empty()) {
      return "error: taskbar-cycle must be invoked from a taskbar widget gesture\n";
    }
    auto* taskbar = findTaskbarWidget(*context);
    if (taskbar == nullptr) {
      return "error: no taskbar widget named '" + context->widgetName + "' on bar '" + context->barName + "'\n";
    }
    taskbar->cycleAdjacent(parts[0] == "next" ? 1 : -1);
    return "ok\n";
  });

  ipc.bind(noctalia::cli::msg::barShow, [this](const std::string& args) -> std::string { return showBarIpc(args); });

  ipc.bind(noctalia::cli::msg::barHide, [this](const std::string& args) -> std::string { return hideBarIpc(args); });

  ipc.bind(noctalia::cli::msg::barToggle, [this](const std::string& args) -> std::string {
    return toggleBarIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barReserveToggle, [this](const std::string& args) -> std::string {
    return toggleBarReserveSpaceIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barAutoHideSet, [this](const std::string& args) -> std::string {
    return setBarAutoHideIpc(args);
  });

  ipc.bind(noctalia::cli::msg::barLayerSet, [this](const std::string& args) -> std::string {
    return setBarLayerIpc(args);
  });
}
