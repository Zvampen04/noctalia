#include "shell/bar/widget.h"

#include "core/files/file_watcher.h"
#include "core/log.h"
#include "cursor-shape-v1-client-protocol.h"
#include "dbus/upower/upower_service.h"
#include "render/animation/animation_manager.h"
#include "render/scene/countdown_ring_node.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "shell/bar/widget_action_dispatcher.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "system/system_monitor_service.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

struct Widget::RingState {
  CountdownRingNode* node = nullptr;
  std::string source;
  SystemMonitorService* monitor = nullptr;
  UPowerService* battery = nullptr;
  FileWatcher* watcher = nullptr;
  std::vector<std::uint64_t> watches;
  WidgetRingUsageColors colors;
  float progress = 0.0F;
  bool externalFrame = false;
  bool frameVisible = true;
  bool failed = false;
  ~RingState() {
    if (monitor && source == "gpu")
      monitor->releaseGpuUsage();
    if (watcher)
      for (auto id : watches)
        if (id)
          watcher->unwatch(id);
  }
  void refreshUpdates() {
    auto read = [](const char* path) {
      std::ifstream stream(path);
      try {
        return nlohmann::json::parse(stream);
      } catch (...) {
        return nlohmann::json::object();
      }
    };
    const auto status = read("/var/lib/nixos-auto-update-switch/status.json");
    const auto session = read("/var/lib/nixos-auto-update-switch/item-session.json");
    const auto state = status.value("status", std::string{});
    const auto phase = status.value("phase", std::string{});
    failed = state == "failed";
    progress = state == "success"
            && (phase == "up-to-date" || phase == "no-changes" || phase == "switched" || phase == "completed")
        ? 1.0F
        : 0.0F;
    const auto& data = status.contains("items") ? status : session;
    if (state != "running" || !data.contains("items") || !data["items"].is_array())
      return;
    auto first = std::numeric_limits<std::int64_t>::max();
    for (const auto& item : data["items"]) {
      if (!item.is_object() || !item.contains("progress") || !item["progress"].is_number())
        continue;
      const auto itemState = item.value("status", std::string{});
      if (itemState != "running" && itemState != "selected" && itemState != "accepted")
        continue;
      const auto percent = item["progress"].get<double>();
      if (!std::isfinite(percent) || percent >= 100.0)
        continue;
      const auto order = item.contains("progress_order") && item["progress_order"].is_number_integer()
          ? item["progress_order"].get<std::int64_t>()
          : std::int64_t{0};
      if (order < first) {
        first = order;
        progress = std::clamp(static_cast<float>(percent / 100.0), 0.0F, 1.0F);
      }
    }
  }
};

Widget::Widget() = default;

void Widget::configureRing(
    bool enabled, std::string source, const WidgetRingUsageColors& colors, SystemMonitorService* monitor, UPowerService* battery, FileWatcher* watcher
) {
  if (!enabled)
    return;
  m_ringState = std::make_unique<RingState>();
  auto& state = *m_ringState;
  state.source = std::move(source);
  state.colors = colors;
  state.monitor = monitor;
  state.battery = battery;
  state.watcher = watcher;
  if (monitor && state.source == "gpu")
    monitor->retainGpuUsage();
  if (state.source == "update_progress") {
    state.refreshUpdates();
    if (watcher)
      for (const auto* path :
           {"/var/lib/nixos-auto-update-switch/status.json", "/var/lib/nixos-auto-update-switch/item-session.json"})
        state.watches.push_back(watcher->watch(
            path,
            [this] {
              m_ringState->refreshUpdates();
              requestUpdate();
            },
            FileWatcher::WatchTrigger::WriteCompleted
        ));
  }
}

void Widget::updateRing() {
  if (!m_ringState || !m_ringState->node)
    return;
  auto& state = *m_ringState;
  float progress = 0.0F;
  bool critical = false;
  if (state.source == "battery" && state.battery) {
    const auto battery = state.battery->state();
    progress = battery.isPresent ? static_cast<float>(battery.percentage / 100.0) : 0.0F;
    critical = battery.isPresent && battery.percentage <= 15.0;
  } else if (state.source == "update_progress") {
    progress = state.progress;
    critical = state.failed;
  } else if (state.monitor) {
    const auto stats = state.monitor->latest();
    if (state.source == "cpu")
      progress = static_cast<float>(stats.cpuUsagePercent / 100.0);
    if (state.source == "ram")
      progress = static_cast<float>(stats.ramUsagePercent / 100.0);
    if (state.source == "gpu")
      progress = static_cast<float>(stats.gpuUsagePercent.value_or(0.0) / 100.0);
  }
  state.node->setProgress(std::isfinite(progress) ? std::clamp(progress, 0.0F, 1.0F) : 0.0F);
  state.node->setThickness(1.2F * m_contentScale);
  const bool usage = state.source == "cpu" || state.source == "ram" || state.source == "gpu";
  state.node->setColor(usage && state.colors.enabled
      ? resolveColorSpec(state.colors.colorForPercent(state.node->style().progress * 100.F))
      : colorForRole(critical ? ColorRole::Error : ColorRole::Primary));
}

namespace {

  constexpr float kCapsuleInkEpsilon = 0.5F;
  constexpr Logger kLog("bar.actions");

} // namespace

Widget::~Widget() {
  if (m_animations != nullptr) {
    m_animations->cancelForOwner(this);
  }
}

ColorSpec Widget::widgetForegroundOr(const ColorSpec& fallback) const noexcept {
  // Per-widget `color` must win over bar/widget `capsule_foreground`, otherwise a bar-level
  // capsule_foreground (e.g. on_primary) overrides explicit `color = primary` after layout.
  if (m_widgetForeground.has_value()) {
    return *m_widgetForeground;
  }
  const auto& spec = m_barCapsuleSpec;
  if (spec.enabled && spec.foreground.has_value()) {
    return *spec.foreground;
  }
  return fallback;
}

ColorSpec Widget::widgetIconColorOr(const ColorSpec& fallback) const noexcept {
  // `icon_color` overrides; otherwise the icon inherits the full foreground chain
  // (`color` → `capsule_foreground` → fallback), so a bare `color` still tints icons.
  if (m_widgetIconColor.has_value()) {
    return *m_widgetIconColor;
  }
  return widgetForegroundOr(fallback);
}

bool Widget::shouldShowBarCapsule() const {
  if (!m_barCapsuleSpec.enabled) {
    return false;
  }
  const Node* r = root();
  if (r == nullptr || !r->visible()) {
    return false;
  }
  if (r->width() <= kCapsuleInkEpsilon || r->height() <= kCapsuleInkEpsilon) {
    return false;
  }
  // No scene children ⇒ nothing to frame (spacer bare node, empty tray flex, etc.).
  if (r->children().empty()) {
    return false;
  }
  return true;
}

float Widget::resolvedBarCapsuleRadius(float width, float height) const noexcept {
  const float maxRadius = std::max(0.0F, std::min(width, height) * 0.5F);
  if (!m_barCapsuleSpec.radius.has_value()) {
    return maxRadius;
  }
  return std::clamp(*m_barCapsuleSpec.radius * m_contentScale, 0.0F, maxRadius);
}

void Widget::setBarCapsuleScene(Node* shell, Box* box) noexcept {
  m_capsuleShell = shell;
  m_capsuleBox = box;
}

void Widget::setNonInteractive(bool nonInteractive) noexcept {
  m_nonInteractive = nonInteractive;
  syncOuterHitTestVisible();
  updateGestureAreaEnabled();
}

void Widget::setBarPointerSuppressed(bool suppressed) noexcept {
  if (m_barPointerSuppressed == suppressed) {
    return;
  }
  m_barPointerSuppressed = suppressed;
  syncOuterHitTestVisible();
}

void Widget::syncOuterHitTestVisible() noexcept {
  if (Node* outer = outerNode(); outer != nullptr) {
    outer->setHitTestVisible(!m_nonInteractive && !m_barPointerSuppressed);
  }
}

void Widget::updateGestureAreaEnabled() noexcept {
  if (m_gestureArea != nullptr) {
    m_gestureArea->setEnabled(!m_nonInteractive && !m_gestureBindings.empty());
  }
}

float Widget::width() const noexcept { return outerNode() != nullptr ? outerNode()->width() : 0.0F; }

float Widget::height() const noexcept { return outerNode() != nullptr ? outerNode()->height() : 0.0F; }

std::unique_ptr<Node> Widget::releaseRoot() {
  m_outerPtr = m_outer.get();
  return std::move(m_outer);
}

void Widget::setRoot(std::unique_ptr<Node> root) {
  m_innerRoot = root.get();
  m_innerArea = dynamic_cast<InputArea*>(m_innerRoot);
  m_innerBaseButtons = m_innerArea != nullptr ? m_innerArea->acceptedButtons() : 0;
  m_innerBaseScrollDirections = m_innerArea != nullptr ? m_innerArea->acceptedScrollDirections() : 0;

  auto gestureArea = ui::inputArea({});
  m_gestureArea = gestureArea.get();
  // Nothing is bound until resolveGestureBindings() runs, and an area with no accepted buttons
  // never wins the dispatcher's ancestor walk.
  m_gestureArea->setAcceptedButtons(0);
  if (root != nullptr) {
    m_gestureArea->addChild(std::move(root));
  }

  if (m_ringState) {
    auto ring = std::make_unique<CountdownRingNode>();
    m_ringState->node = ring.get();
    ring->setSymmetric(true);
    ring->setHitTestVisible(false);
    ring->setParticipatesInLayout(false);
    gestureArea->addChild(std::move(ring));
  }
  m_outer = std::move(gestureArea);
  m_outer->setHitTestVisible(!m_nonInteractive && !m_barPointerSuppressed);
  // Bindings are resolved before create() runs, so install them here too: whichever of the two
  // happens last is the one that wires the area up.
  installGestureHandlers();
  syncOuterFromRoot();
}

void Widget::syncOuterFromRoot() noexcept {
  Node* outer = outerNode();
  if (outer == nullptr || m_innerRoot == nullptr) {
    return;
  }
  const float inset = m_ringState ? 6.0F * m_contentScale : 0.0F;
  m_innerRoot->setPosition(inset, inset);
  outer->setSize(m_innerRoot->width() + 2.0F * inset, m_innerRoot->height() + 2.0F * inset);
  if (m_ringState && m_ringState->node) {
    if (!m_ringState->externalFrame)
      m_ringState->node->setSize(outer->width(), outer->height());
    m_ringState->node->setVisible(m_ringState->frameVisible && m_innerRoot->width() > 0 && m_innerRoot->height() > 0);
  }
  outer->setVisible(m_innerRoot->visible());
  outer->setParticipatesInLayout(m_innerRoot->participatesInLayout());
}

std::optional<CountdownRingStyle> Widget::usageRingStyle() const {
  return m_ringState && m_ringState->node
      ? std::optional{m_ringState->node->style()} : std::nullopt;
}

void Widget::setUsageRingFrame(float x, float y, float width, float height, float radius, bool visible) {
  if (!m_ringState || !m_ringState->node) return;
  m_ringState->externalFrame = true;
  m_ringState->frameVisible = visible;
  auto* ring = m_ringState->node;
  const float inset = 3.0F * m_contentScale;
  ring->setPosition(x + inset, y + inset);
  ring->setSize(std::max(0.0F, width - 2.0F * inset), std::max(0.0F, height - 2.0F * inset));
  ring->setRadius(std::max(0.0F, std::min({radius, width * 0.5F, height * 0.5F}) - inset));
  ring->setVisible(visible && width > 2.0F * inset && height > 2.0F * inset);
}

void Widget::setAnimationManager(AnimationManager* mgr) noexcept { m_animations = mgr; }

void Widget::setUpdateCallback(UpdateCallback callback) { m_updateCallback = std::move(callback); }

void Widget::setRedrawCallback(RedrawCallback callback) { m_redrawCallback = std::move(callback); }

void Widget::setFrameTickRequestCallback(FrameTickRequestCallback callback) {
  m_frameTickRequestCallback = std::move(callback);
}

void Widget::setPanelToggleCallback(PanelToggleCallback callback) { m_panelToggleCallback = std::move(callback); }

void Widget::requestUpdate() {
  if (m_updateCallback) {
    m_updateCallback();
  }
}

void Widget::requestRedraw() {
  if (m_redrawCallback) {
    m_redrawCallback();
  }
}

void Widget::requestFrameTick() {
  if (m_frameTickRequestCallback) {
    m_frameTickRequestCallback();
  }
}

void Widget::requestPanelToggle(
    std::string_view panelId, std::string_view context, std::optional<float> anchorSurfaceX,
    std::optional<float> anchorSurfaceY, PanelActivation activation
) {
  if (m_panelToggleCallback) {
    m_panelToggleCallback(panelId, context, anchorSurfaceX, anchorSurfaceY, activation);
  }
}

void Widget::applyCommonOptions(
    const CommonWidgetOptions& options, FontWeight barFontWeight, const std::string& barFontFamily,
    std::string_view logContext
) {
  setAnchor(options.anchor);
  setNonInteractive(!options.interactive);
  m_labelFontWeight =
      options.labelFontWeight.has_value() ? static_cast<FontWeight>(*options.labelFontWeight) : barFontWeight;
  std::string labelFontFamily = StringUtils::trim(options.labelFontFamily);
  if (labelFontFamily.empty()) {
    m_labelFontFamily = barFontFamily;
  } else {
    m_labelFontFamily = std::move(labelFontFamily);
  }

  m_fontScale = options.fontScale;

  m_scrollRepeatMode = noctalia::bar::ScrollRepeatMode::Auto;
  if (const auto mode = noctalia::bar::parseScrollRepeatMode(options.scrollRepeat); mode.has_value()) {
    m_scrollRepeatMode = *mode;
  } else {
    kLog.error("{}.scroll_repeat: unknown mode \"{}\"", logContext, options.scrollRepeat);
  }
}

void Widget::resolveGestureBindings(
    std::string_view widgetType, const WidgetConfig* widgetConfig,
    const noctalia::bar::WidgetActionBindings::ActionTable* barActions, std::string_view barContext,
    const noctalia::bar::WidgetActionDispatcher* dispatcher
) {
  m_actionDispatcher = dispatcher;

  const std::string widgetContext = std::format("widget.{}", m_configName);
  // Named, not inlined: the span in Inputs borrows from it.
  const auto typeDefaults = noctalia::bar::gestureDefaultsForType(widgetType, widgetConfig);
  m_gestureBindings.resolve(
      noctalia::bar::WidgetActionBindings::Inputs{
          .builtinDefaults = noctalia::bar::builtinGestureDefaults(),
          .widgetDefaults = typeDefaults,
          .barActions = barActions,
          .widgetActions = noctalia::bar::findActionTable(widgetConfig),
          .reserved = noctalia::bar::reservedGesturesForType(widgetType),
          .widgetContext = widgetContext,
          .barContext = barContext,
          .widgetName = m_configName,
          .widgetType = widgetType,
      }
  );

  installGestureHandlers();
}

void Widget::installGestureHandlers() {
  if (m_gestureArea == nullptr) {
    return;
  }

  const auto bound = m_gestureBindings.boundGestures();

  std::uint32_t mask = 0;
  std::uint32_t scrollMask = 0;
  for (const auto gesture : noctalia::bar::allGestures()) {
    if (!bound.contains(gesture)) {
      continue;
    }
    for (const auto button : noctalia::bar::buttonsForGesture(gesture)) {
      mask |= InputArea::buttonMask(button);
    }
    if (const auto direction = noctalia::bar::scrollDirectionForGesture(gesture); direction.has_value()) {
      scrollMask |= InputArea::scrollDirectionMask(*direction);
    }
  }
  m_gestureArea->setAcceptedButtons(mask);
  updateGestureAreaEnabled();

  if (m_innerArea != nullptr) {
    // Both dispatcher walks start at the innermost area, so the widget's own root has to give up
    // whatever the wrapper is bound to. This is what lets a config binding override a gesture the
    // widget still handles itself, for scroll as much as for clicks.
    m_innerArea->setAcceptedButtons(m_innerBaseButtons & ~mask);
    m_innerArea->setAcceptedScrollDirections(m_innerBaseScrollDirections & ~scrollMask);
    // Hover resolves to the innermost area, so it carries the pointer cursor for the wrapper.
    if (mask != 0 && m_innerArea->cursorShape() == 0) {
      m_innerArea->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER);
    }
  }

  // Runs once per widget per reload; the resolved set is the first thing to check when a binding
  // does not fire.
  std::string summary;
  for (const auto gesture : noctalia::bar::allGestures()) {
    const auto* action = m_gestureBindings.find(gesture);
    if (action == nullptr) {
      continue;
    }
    if (!summary.empty()) {
      summary += ", ";
    }
    summary += std::format(
        "{}={}", gestureConfigKey(gesture),
        action->kind == noctalia::bar::WidgetAction::Kind::Exec ? std::format("exec {}", action->args)
                                                                : action->commandLine()
    );
  }
  kLog.debug("widget.{}: {}", m_configName, summary.empty() ? "no gesture bindings" : summary);

  if (mask != 0) {
    m_gestureArea->setOnClick([this](const InputArea::PointerData& data) {
      if (const auto gesture = noctalia::bar::gestureForButton(data.button)) {
        dispatchGesture(*gesture);
      }
    });
  }

  m_gestureArea->setOnAxisHandler([this](const InputArea::PointerData& data) {
    const auto gesture = noctalia::bar::gestureForScroll(data.axis, data.scrollSteps());
    if (!gesture.has_value()) {
      // Report a scroll gesture we own as consumed even between detents, so a partly accumulated
      // flick does not leak to an ancestor mid-gesture.
      return m_gestureBindings.find(noctalia::bar::Gesture::ScrollUp) != nullptr
          || m_gestureBindings.find(noctalia::bar::Gesture::ScrollDown) != nullptr;
    }
    if (!data.scrollStepStartsGesture() && !bindingRepeatsEveryScrollStep(*gesture)) {
      return true;
    }
    return dispatchGesture(*gesture);
  });
}

bool Widget::bindingRepeatsEveryScrollStep(noctalia::bar::Gesture gesture) const {
  const auto* action = m_gestureBindings.find(gesture);
  const bool actionCycles = action != nullptr && m_actionDispatcher != nullptr && m_actionDispatcher->cycles(*action);
  return noctalia::bar::scrollRepeatsEveryStep(m_scrollRepeatMode, actionCycles);
}

bool Widget::dispatchGesture(noctalia::bar::Gesture gesture) {
  const auto* action = m_gestureBindings.find(gesture);
  if (action == nullptr) {
    return false;
  }

  onGestureDispatch(gesture, *action);

  // Panel actions re-enter through the bar's panel callback so the panel anchors at this widget
  // rather than at the compositor's focused output.
  if (action->kind == noctalia::bar::WidgetAction::Kind::Ipc && noctalia::bar::isAnchoredPanelVerb(action->verb)) {
    const auto args = noctalia::bar::parsePanelVerbArgs(action->args);
    if (args.panelId.empty()) {
      kLog.error(
          "widget.{}.actions.{}: \"{}\" needs a panel id", m_configName, gestureConfigKey(gesture), action->verb
      );
      return false;
    }
    const auto activation =
        action->verb == "panel-open" ? Widget::PanelActivation::Open : Widget::PanelActivation::Toggle;
    requestPanelToggle(args.panelId, args.panelContext, std::nullopt, std::nullopt, activation);
    return true;
  }

  if (m_actionDispatcher == nullptr) {
    return false;
  }

  // The dispatcher reports the command and the failure itself.
  return m_actionDispatcher->run(*action, m_actionContext);
}
