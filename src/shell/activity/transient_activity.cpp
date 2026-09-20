#include "shell/activity/transient_activity.h"

#include "config/config_types.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace {

  using namespace std::literals;

  using RouteConfig = ActivityRouteConfig;

  template <typename Enum, std::size_t N>
  std::optional<Enum> parseValue(std::string_view value, const std::array<std::pair<std::string_view, Enum>, N>& values) {
    const auto it = std::ranges::find(values, value, &std::pair<std::string_view, Enum>::first);
    return it == values.end() ? std::nullopt : std::optional{it->second};
  }

  constexpr std::array kPresentations{
      std::pair{"standalone"sv, TransientActivityPresentation::Standalone},
      std::pair{"section"sv, TransientActivityPresentation::Section},
      std::pair{"attached"sv, TransientActivityPresentation::Attached},
  };
  constexpr std::array kPlacements{
      std::pair{"auto"sv, TransientActivityPlacement::Auto},
      std::pair{"before"sv, TransientActivityPlacement::Before},
      std::pair{"after"sv, TransientActivityPlacement::After},
      std::pair{"below"sv, TransientActivityPlacement::Below},
      std::pair{"above"sv, TransientActivityPlacement::Above},
      std::pair{"left"sv, TransientActivityPlacement::Left},
      std::pair{"right"sv, TransientActivityPlacement::Right},
  };
  constexpr std::array kMotions{
      std::pair{"inherit"sv, TransientActivityMotion::Inherit},
      std::pair{"off"sv, TransientActivityMotion::Off},
  };
  constexpr std::array kMaterials{
      std::pair{"inherit"sv, TransientActivityMaterial::Inherit},
      std::pair{"surface"sv, TransientActivityMaterial::Surface},
      std::pair{"transparent"sv, TransientActivityMaterial::Transparent},
  };

  const ActivityRouteOverrideConfig& overrideFor(const OsdActivityConfig& config, TransientActivityKind kind) {
    switch (kind) {
    case TransientActivityKind::VolumeOutput:
    case TransientActivityKind::VolumeInput:
      return config.volume;
    case TransientActivityKind::Brightness:
      return config.brightness;
    case TransientActivityKind::Notification:
      return config.notification;
    }
    return config.volume;
  }

  RouteConfig baseRoute(const OsdActivityConfig& config) {
    return RouteConfig{
        .presentation = config.presentation,
        .bar = config.bar,
        .section = config.section,
        .output = config.output,
        .placement = config.placement,
        .motion = config.motion,
        .material = config.material,
        .width = config.width, .height = config.height, .progressThickness = config.progressThickness,
        .timeoutMs = config.timeoutMs, .showBody = config.showBody,
    };
  }

  RouteConfig mergedRoute(const OsdActivityConfig& config, TransientActivityKind kind) {
    RouteConfig route = baseRoute(config);
    const auto& over = overrideFor(config, kind);
    if (!over.presentation.empty()) route.presentation = over.presentation;
    if (!over.bar.empty()) route.bar = over.bar;
    if (!over.section.empty()) route.section = over.section;
    if (!over.output.empty()) route.output = over.output;
    if (!over.placement.empty()) route.placement = over.placement;
    if (!over.motion.empty()) route.motion = over.motion;
    if (!over.material.empty()) route.material = over.material;
    if (over.width > 0) route.width = std::max(120,over.width);
    if (over.height > 0) route.height = std::max(24,over.height);
    if (over.progressThickness > 0) route.progressThickness = over.progressThickness;
    if (over.timeoutMs > 0) route.timeoutMs = over.timeoutMs;
    if (!over.body.empty()) route.showBody = over.body == "show";
    return route;
  }

  std::optional<std::string> validateRoute(const RouteConfig& route, std::string_view path, bool allowEmpty) {
    const auto valid = [allowEmpty](const std::string& value, const auto& values) {
      return (allowEmpty && value.empty()) || parseValue(value, values).has_value();
    };
    if (!valid(route.presentation, kPresentations)) return std::string(path) + ".presentation";
    if (!valid(route.placement, kPlacements)) return std::string(path) + ".placement";
    if (!valid(route.motion, kMotions)) return std::string(path) + ".motion";
    if (!valid(route.material, kMaterials)) return std::string(path) + ".material";
    if (!allowEmpty && route.output.empty()) return std::string(path) + ".output";
    if (route.presentation == "section" && route.section.empty()) return std::string(path) + ".section";
    return std::nullopt;
  }

  std::optional<std::string>
  validateOverride(const ActivityRouteOverrideConfig& route, std::string_view path) {
    if (!route.presentation.empty() && !parseValue(route.presentation, kPresentations))
      return std::string(path) + ".presentation";
    if (!route.placement.empty() && !parseValue(route.placement, kPlacements))
      return std::string(path) + ".placement";
    if (!route.motion.empty() && !parseValue(route.motion, kMotions))
      return std::string(path) + ".motion";
    if (!route.body.empty() && route.body != "show" && route.body != "hide") return std::string(path) + ".body";
    if (!route.material.empty() && !parseValue(route.material, kMaterials))
      return std::string(path) + ".material";
    return std::nullopt;
  }

} // namespace

TransientActivityRoute resolveTransientActivityRoute(const OsdActivityConfig& config, TransientActivityKind kind) {
  const RouteConfig route = mergedRoute(config, kind);
  return {
      .presentation = parseValue(route.presentation, kPresentations).value_or(TransientActivityPresentation::Standalone),
      .bar = route.bar,
      .section = route.section,
      .output = route.output.empty() ? "focused" : route.output,
      .placement = parseValue(route.placement, kPlacements).value_or(TransientActivityPlacement::Auto),
      .motion = parseValue(route.motion, kMotions).value_or(TransientActivityMotion::Inherit),
      .material = parseValue(route.material, kMaterials).value_or(TransientActivityMaterial::Inherit),
      .width = route.width, .height = route.height, .progressThickness = route.progressThickness,
      .timeoutMs = route.timeoutMs, .showBody = route.showBody,
  };
}

std::optional<std::string> validateTransientActivityConfig(const OsdActivityConfig& config) {
  if (auto error = validateRoute(baseRoute(config), "osd.activity", false))
    return error;
  for (const auto [route, path] : std::array{
           std::pair{&config.volume, "osd.activity.volume"sv},
           std::pair{&config.brightness, "osd.activity.brightness"sv},
           std::pair{&config.notification, "osd.activity.notification"sv},
       }) {
    if (auto error = validateOverride(*route, path)) return error;
    if (const auto merged = mergedRoute(config, route == &config.volume
                ? TransientActivityKind::VolumeOutput
                : route == &config.brightness ? TransientActivityKind::Brightness
                                              : TransientActivityKind::Notification);
        merged.presentation == "section" && merged.section.empty())
      return std::string(path) + ".section";
  }
  return std::nullopt;
}

void TransientActivityService::setPresenter(Presenter presenter) {
  if (m_active && m_presenter.withdraw) m_presenter.withdraw(m_active->model.serial);
  m_presenter = std::move(presenter);
  refreshPresentation();
}

void TransientActivityService::setFallback(Fallback fallback) { m_fallback = std::move(fallback); }

bool TransientActivityService::publish(
    TransientActivityViewModel model, const TransientActivityRoute& route, Clock::time_point now) {
  const std::uint64_t generation = ++m_generation;
  if (route.presentation == TransientActivityPresentation::Standalone || !routeAvailable(route)) return false;

  if (route.timeoutMs > 0) model.timeout = std::chrono::milliseconds(route.timeoutMs);
  model.serial = m_nextSerial++;
  model.progress = std::clamp(model.progress, 0.0F, 1.5F);
  model.expiresAt = model.timeout.count() > 0 ? now + model.timeout : Clock::time_point::max();
  Entry incoming{.model = std::move(model), .route = route};

  pruneExpired(now);
  if (m_active && m_active->model.sourceKey == incoming.model.sourceKey) {
    incoming.model.serial = m_active->model.serial;
    Entry previous = *m_active;
    m_active = std::move(incoming);
    const bool accepted = present(*m_active);
    if (m_generation != generation) return accepted;
    if (!accepted) {
      rejectPresentation(std::move(previous), now, false);
      return false;
    }
    releaseHover(previous, now);
    if (m_generation != generation) return true;
    armTimer(now);
    return true;
  }

  auto queued = std::ranges::find(m_pending, incoming.model.sourceKey, [](const Entry& item) {
    return item.model.sourceKey;
  });
  if (queued != m_pending.end()) {
    incoming.model.serial = queued->model.serial;
    *queued = std::move(incoming);
    armTimer(now);
    return true;
  }

  if (!m_active) {
    m_active = std::move(incoming);
    const bool accepted = present(*m_active);
    if (m_generation != generation) return accepted;
    if (!accepted) {
      m_active.reset();
      return false;
    }
    armTimer(now);
    return true;
  }

  if (incoming.model.priority > m_active->model.priority) {
    Entry interrupted = std::move(*m_active);
    m_active = std::move(incoming);
    const bool accepted = present(*m_active);
    if (m_generation != generation) return accepted;
    if (!accepted) {
      rejectPresentation(std::move(interrupted), now, true);
      return false;
    }
    releaseHover(interrupted, now);
    if (m_generation != generation) return true;
    if (interrupted.model.expiresAt > now) m_pending.push_front(std::move(interrupted));
  } else {
    m_pending.push_back(std::move(incoming));
  }
  armTimer(now);
  return true;
}

void TransientActivityService::closeNotification(std::uint32_t notificationId, Clock::time_point now) {
  ++m_generation;
  if (m_active && m_active->model.notificationId == notificationId) {
    Entry closed = std::move(*m_active);
    m_active.reset();
    releaseHover(closed, now);
    if (m_presenter.withdraw) m_presenter.withdraw(closed.model.serial);
  }
  std::erase_if(m_pending, [notificationId](const Entry& entry) {
    return entry.model.notificationId == notificationId;
  });
  promote(now);
}

void TransientActivityService::expire(Clock::time_point now) {
  ++m_generation;
  pruneExpired(now);
  promote(now);
}

void TransientActivityService::reloadConfig(const OsdActivityConfig& config, Clock::time_point now) {
  ++m_generation;
  if (m_active) m_active->route = resolveTransientActivityRoute(config, m_active->model.kind);
  for (auto& entry : m_pending) entry.route = resolveTransientActivityRoute(config, entry.model.kind);
  refreshPresentation(now);
}

void TransientActivityService::refreshPresentation(Clock::time_point now) {
  const std::uint64_t generation = ++m_generation;
  pruneExpired(now);
  if (!m_active) {
    promote(now);
    return;
  }
  releaseHover(*m_active, now);
  if (m_generation != generation || !m_active) return;
  const std::uint64_t serial = m_active->model.serial;
  const bool available = routeAvailable(m_active->route);
  const bool accepted = available && present(*m_active);
  if (m_generation != generation || !m_active || m_active->model.serial != serial) return;
  if (!accepted) {
    Entry lost = std::move(*m_active);
    m_active.reset();
    releaseHover(lost, now);
    if (m_presenter.withdrawNow) m_presenter.withdrawNow(lost.model.serial);
    else if (m_presenter.withdraw) m_presenter.withdraw(lost.model.serial);
    fallbackOnce(std::move(lost));
  }
  if (!m_active) promote(now);
}

void TransientActivityService::clear() {
  ++m_generation;
  m_timer.stop();
  auto active = std::move(m_active);
  m_active.reset();
  m_pending.clear();
  if (active) {
    releaseHover(*active, Clock::now());
    if (m_presenter.withdraw) m_presenter.withdraw(active->model.serial);
  }
}

const TransientActivityViewModel* TransientActivityService::active() const noexcept {
  return m_active ? &m_active->model : nullptr;
}

bool TransientActivityService::routeAvailable(const TransientActivityRoute& route) const {
  return m_presenter.canPresent && m_presenter.canPresent(route);
}

bool TransientActivityService::present(const Entry& entry) {
  if (!m_presenter.present) return false;
  const auto model = entry.model;
  const auto route = entry.route;
  return m_presenter.present(model, route);
}

void TransientActivityService::releaseHover(Entry& entry, Clock::time_point now) {
  if (!entry.pausedRemaining) return;
  entry.model.expiresAt = now + *entry.pausedRemaining;
  entry.pausedRemaining.reset();
  if (entry.model.hoverChanged) entry.model.hoverChanged(false);
}

void TransientActivityService::rejectPresentation(
    Entry previous, Clock::time_point now, bool fallbackPrevious) {
  // canPresent() and present() are deliberately separate because a Wayland endpoint can
  // disappear while the presenter mounts its real node. Once that happens, retire every
  // record the broker owned before returning control to the producer. Keeping the old
  // attached node while returning false would give the producer and presenter concurrent
  // visual ownership.
  auto pending = std::move(m_pending);
  m_pending.clear();
  m_active.reset();
  m_timer.stop();

  const std::uint64_t generation = m_generation;
  releaseHover(previous, now);
  if (m_presenter.withdrawNow) m_presenter.withdrawNow(previous.model.serial);
  else if (m_presenter.withdraw) m_presenter.withdraw(previous.model.serial);
  if (m_generation != generation) return;
  if (fallbackPrevious) {
    fallbackOnce(std::move(previous));
    if (m_generation != generation) return;
  }
  for (auto& entry : pending) {
    fallbackOnce(std::move(entry));
    if (m_generation != generation) return;
  }
}

void TransientActivityService::setHovered(std::uint64_t serial, bool hovered, Clock::time_point now) {
  ++m_generation;
  if (!m_active || m_active->model.serial != serial || m_active->model.timeout.count() <= 0) return;
  if (hovered) {
    if (m_active->pausedRemaining) return;
    m_active->pausedRemaining = std::max(
        std::chrono::milliseconds{1},
        std::chrono::ceil<std::chrono::milliseconds>(m_active->model.expiresAt - now));
    m_active->model.expiresAt = Clock::time_point::max();
  } else {
    if (!m_active->pausedRemaining) return;
    m_active->model.expiresAt = now + *m_active->pausedRemaining;
    m_active->pausedRemaining.reset();
  }
  armTimer(now);
}

void TransientActivityService::armTimer(Clock::time_point now) {
  Clock::time_point next = Clock::time_point::max();
  if (m_active) next = m_active->model.expiresAt;
  for (const auto& entry : m_pending) next = std::min(next, entry.model.expiresAt);
  if (next == Clock::time_point::max()) {
    m_timer.stop();
    return;
  }
  const auto delay = std::max(std::chrono::milliseconds(0),
      std::chrono::ceil<std::chrono::milliseconds>(next - now));
  m_timer.start(delay, [this]() { expire(); });
}

void TransientActivityService::fallbackOnce(Entry entry) {
  if (m_fallback) m_fallback(entry.model);
}

void TransientActivityService::promote(Clock::time_point now) {
  pruneExpired(now);
  while (!m_active && !m_pending.empty()) {
    auto best = std::ranges::max_element(m_pending, {}, [](const Entry& entry) { return entry.model.priority; });
    Entry next = std::move(*best);
    m_pending.erase(best);
    const std::uint64_t generation = m_generation;
    const bool accepted = routeAvailable(next.route) && present(next);
    if (m_generation != generation) return;
    if (!accepted) {
      fallbackOnce(std::move(next));
      continue;
    }
    m_active = std::move(next);
  }
  armTimer(now);
}

void TransientActivityService::pruneExpired(Clock::time_point now) {
  if (m_active && m_active->model.expiresAt <= now) {
    Entry expired = std::move(*m_active);
    m_active.reset();
    releaseHover(expired, now);
    if (m_presenter.withdraw) m_presenter.withdraw(expired.model.serial);
  }
  std::erase_if(m_pending, [now](const Entry& entry) { return entry.model.expiresAt <= now; });
}
