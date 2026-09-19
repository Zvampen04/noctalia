#include "config/config_types.h"
#include "shell/activity/transient_activity.h"

#include <cassert>
#include <chrono>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {
  TransientActivityViewModel model(std::string key, int priority, std::chrono::milliseconds timeout = 2s) {
    return {.sourceKey = std::move(key), .priority = priority, .timeout = timeout};
  }
}

int main() {
  OsdActivityConfig config;
  assert(resolveTransientActivityRoute(config, TransientActivityKind::VolumeOutput).presentation
      == TransientActivityPresentation::Standalone);
  config.presentation = "attached";
  config.placement = "auto";
  config.motion = "off";
  config.material = "inherit";
  config.bar = "terminal";
  config.volume.placement = "below";
  const auto terminal = resolveTransientActivityRoute(config, TransientActivityKind::VolumeOutput);
  assert(terminal.presentation == TransientActivityPresentation::Attached);
  assert(terminal.bar == "terminal" && terminal.placement == TransientActivityPlacement::Below);
  assert(terminal.motion == TransientActivityMotion::Off);
  config.brightness.presentation = "invalid";
  assert(validateTransientActivityConfig(config) == "osd.activity.brightness.presentation");
  config.brightness.presentation.clear();
  assert(!validateTransientActivityConfig(config));
  config.presentation = "section";
  config.section = "status";
  config.brightness.presentation = "section";
  config.brightness.section.clear();
  assert(!validateTransientActivityConfig(config));
  const auto inheritedSection = resolveTransientActivityRoute(config, TransientActivityKind::Brightness);
  assert(inheritedSection.presentation == TransientActivityPresentation::Section);
  assert(inheritedSection.section == "status");

  TransientActivityService service;
  std::vector<std::string> shown;
  std::vector<std::string> fallback;
  bool available = true;
  service.setPresenter({
      .canPresent = [&available](const TransientActivityRoute&) { return available; },
      .present = [&shown](const TransientActivityViewModel& value, const TransientActivityRoute&) {
        shown.push_back(value.sourceKey + ":" + value.value);
        return true;
      },
      .withdraw = {},
  });
  service.setFallback([&fallback](const TransientActivityViewModel& value) { fallback.push_back(value.sourceKey); });

  const auto now = TransientActivityService::Clock::time_point{100s};
  auto ordinary = model("notification:1", kTransientActivityPriorityNotificationNormal, 4s);
  ordinary.notificationId = 1;
  assert(service.publish(ordinary, terminal, now));
  assert(service.active()->sourceKey == "notification:1" && service.pendingCount() == 0);

  auto volume = model("volume-output", kTransientActivityPriorityDevice);
  volume.value = "20%";
  assert(service.publish(volume, terminal, now + 100ms));
  assert(service.active()->sourceKey == "volume-output" && service.pendingCount() == 1);
  auto replacement = model("volume-output", kTransientActivityPriorityDevice);
  replacement.value = "25%";
  assert(service.publish(replacement, terminal, now + 200ms));
  assert(service.active() && service.active()->value == "25%" && service.pendingCount() == 1);

  auto critical = model("notification:2", kTransientActivityPriorityNotificationCritical, 0ms);
  critical.notificationId = 2;
  assert(service.publish(critical, terminal, now + 300ms));
  assert(service.active()->sourceKey == "notification:2" && service.pendingCount() == 2);
  service.closeNotification(2, now + 400ms);
  assert(service.active()->sourceKey == "volume-output");

  available = false;
  service.refreshPresentation(now + 500ms);
  assert(fallback.size() == 2);
  assert(fallback[0] == "volume-output" && fallback[1] == "notification:1");
  auto brightness = model("brightness", kTransientActivityPriorityDevice);
  assert(!service.publish(brightness, terminal, now + 600ms));

  available = true;
  service.expire(now + 10s);
  assert(service.active() == nullptr);

  {
    TransientActivityService reloaded;
    std::vector<std::string> reloadedFallback;
    int withdrawals = 0;
    reloaded.setPresenter({
        .canPresent = [](const TransientActivityRoute& route) {
          return route.presentation != TransientActivityPresentation::Standalone;
        },
        .present = [](const TransientActivityViewModel&, const TransientActivityRoute&) { return true; },
        .withdraw = [&withdrawals](std::uint64_t) { ++withdrawals; },
    });
    reloaded.setFallback([&reloadedFallback](const TransientActivityViewModel& value) {
      reloadedFallback.push_back(value.sourceKey);
    });
    bool producerHovered = false;
    auto embedded = model("volume-output", 20, 2s);
    embedded.hoverChanged = [&producerHovered](bool value) { producerHovered = value; };
    assert(reloaded.publish(embedded, terminal, now));
    const std::uint64_t serial = reloaded.active()->serial;
    reloaded.setHovered(serial, true, now + 50ms);
    reloaded.active()->hoverChanged(true);
    assert(producerHovered);
    OsdActivityConfig standalone;
    reloaded.reloadConfig(standalone, now + 100ms);
    assert(reloaded.active() == nullptr);
    assert(withdrawals == 1);
    assert(!producerHovered);
    assert(reloadedFallback == std::vector<std::string>{"volume-output"});
    reloaded.refreshPresentation(now + 200ms);
    assert(reloadedFallback == std::vector<std::string>{"volume-output"});
  }

  {
    TransientActivityService hovered;
    hovered.setPresenter({
        .canPresent = [](const TransientActivityRoute&) { return true; },
        .present = [](const TransientActivityViewModel&, const TransientActivityRoute&) { return true; },
        .withdraw = {},
    });
    assert(hovered.publish(model("brightness", 20, 2s), terminal, now));
    const std::uint64_t serial = hovered.active()->serial;
    hovered.setHovered(serial, true, now + 500ms);
    hovered.expire(now + 10s);
    assert(hovered.active() != nullptr);
    hovered.setHovered(serial, false, now + 10s);
    hovered.expire(now + 11s);
    assert(hovered.active() != nullptr);
    hovered.expire(now + 12s);
    assert(hovered.active() == nullptr);
  }

  {
    TransientActivityService ordered;
    ordered.setPresenter({
        .canPresent = [](const TransientActivityRoute&) { return true; },
        .present = [](const TransientActivityViewModel&, const TransientActivityRoute&) { return true; },
        .withdraw = {},
    });
    auto blocker = model("notification:99", 100, 0ms);
    blocker.notificationId = 99;
    assert(ordered.publish(blocker, terminal, now));
    auto first = model("notification:10", 40, 4s);
    first.notificationId = 10;
    auto second = model("notification:11", 40, 4s);
    second.notificationId = 11;
    auto closed = model("notification:12", 30, 4s);
    closed.notificationId = 12;
    assert(ordered.publish(first, terminal, now + 100ms));
    assert(ordered.publish(second, terminal, now + 200ms));
    assert(ordered.publish(closed, terminal, now + 300ms));
    assert(ordered.pendingCount() == 3);
    ordered.closeNotification(12, now + 400ms);
    assert(ordered.pendingCount() == 2);
    ordered.closeNotification(99, now + 500ms);
    assert(ordered.active() != nullptr && ordered.active()->sourceKey == "notification:10");
  }

  {
    TransientActivityService reentrant;
    bool clearDuringPresent = false;
    std::vector<std::string> reentrantFallback;
    reentrant.setPresenter({
        .canPresent = [](const TransientActivityRoute&) { return true; },
        .present = [&reentrant, &clearDuringPresent](
                       const TransientActivityViewModel&, const TransientActivityRoute&) {
          if (clearDuringPresent) {
            reentrant.clear();
            return false;
          }
          return true;
        },
        .withdraw = {},
    });
    reentrant.setFallback([&reentrantFallback](const TransientActivityViewModel& value) {
      reentrantFallback.push_back(value.sourceKey);
    });
    assert(reentrant.publish(model("volume-output", 20), terminal, now));
    clearDuringPresent = true;
    reentrant.refreshPresentation(now + 100ms);
    assert(reentrant.active() == nullptr);
    assert(reentrantFallback.empty());
  }

  {
    TransientActivityService rejected;
    bool acknowledge = true;
    int withdrawals = 0;
    std::vector<std::string> rejectedFallback;
    rejected.setPresenter({
        .canPresent = [](const TransientActivityRoute&) { return true; },
        .present = [&acknowledge](const TransientActivityViewModel&, const TransientActivityRoute&) {
          return acknowledge;
        },
        .withdraw = [&withdrawals](std::uint64_t) { ++withdrawals; },
    });
    rejected.setFallback([&rejectedFallback](const TransientActivityViewModel& value) {
      rejectedFallback.push_back(value.sourceKey);
    });
    assert(rejected.publish(model("brightness", 20), terminal, now));
    acknowledge = false;
    rejected.refreshPresentation(now + 100ms);
    assert(rejected.active() == nullptr);
    assert(withdrawals == 1);
    assert(rejectedFallback == std::vector<std::string>{"brightness"});
    rejected.refreshPresentation(now + 200ms);
    assert(rejectedFallback == std::vector<std::string>{"brightness"});
  }

  {
    TransientActivityService rejectedReplacement;
    bool acknowledge = true;
    std::vector<std::uint64_t> withdrawn;
    std::vector<std::string> rejectedFallback;
    rejectedReplacement.setPresenter({
        .canPresent = [](const TransientActivityRoute&) { return true; },
        .present = [&acknowledge](const TransientActivityViewModel&, const TransientActivityRoute&) {
          return acknowledge;
        },
        .withdraw = {},
        .withdrawNow = [&withdrawn](std::uint64_t serial) { withdrawn.push_back(serial); },
    });
    rejectedReplacement.setFallback([&rejectedFallback](const TransientActivityViewModel& value) {
      rejectedFallback.push_back(value.sourceKey);
    });
    assert(rejectedReplacement.publish(model("volume-output", 60), terminal, now));
    const std::uint64_t oldSerial = rejectedReplacement.active()->serial;
    assert(rejectedReplacement.publish(model("notification:queued", 40), terminal, now + 10ms));
    acknowledge = false;
    assert(!rejectedReplacement.publish(model("volume-output", 60), terminal, now + 20ms));
    assert(rejectedReplacement.active() == nullptr && rejectedReplacement.pendingCount() == 0);
    assert(withdrawn == std::vector<std::uint64_t>{oldSerial});
    // The producer owns the rejected replacement after false; only the independently queued
    // record transfers to the native fallback path.
    assert(rejectedFallback == std::vector<std::string>{"notification:queued"});
  }

  {
    TransientActivityService rejectedPreemption;
    bool acknowledge = true;
    std::vector<std::uint64_t> withdrawn;
    std::vector<std::string> rejectedFallback;
    rejectedPreemption.setPresenter({
        .canPresent = [](const TransientActivityRoute&) { return true; },
        .present = [&acknowledge](const TransientActivityViewModel&, const TransientActivityRoute&) {
          return acknowledge;
        },
        .withdraw = {},
        .withdrawNow = [&withdrawn](std::uint64_t serial) { withdrawn.push_back(serial); },
    });
    rejectedPreemption.setFallback([&rejectedFallback](const TransientActivityViewModel& value) {
      rejectedFallback.push_back(value.sourceKey);
    });
    assert(rejectedPreemption.publish(model("notification:normal", 40), terminal, now));
    const std::uint64_t oldSerial = rejectedPreemption.active()->serial;
    assert(rejectedPreemption.publish(model("notification:low", 30), terminal, now + 10ms));
    acknowledge = false;
    assert(!rejectedPreemption.publish(model("volume-output", 60), terminal, now + 20ms));
    assert(rejectedPreemption.active() == nullptr && rejectedPreemption.pendingCount() == 0);
    assert(withdrawn == std::vector<std::uint64_t>{oldSerial});
    // The interrupted and queued records each retain exactly one native owner. The caller owns
    // fallback for the rejected higher-priority record.
    assert(rejectedFallback == std::vector<std::string>({"notification:normal", "notification:low"}));
  }

  {
    TransientActivityService hoverReentrant;
    bool clearOnRelease = false;
    std::vector<std::string> hoverFallback;
    hoverReentrant.setPresenter({
        .canPresent = [](const TransientActivityRoute&) { return true; },
        .present = [](const TransientActivityViewModel&, const TransientActivityRoute&) { return true; },
        .withdraw = {},
    });
    hoverReentrant.setFallback([&hoverFallback](const TransientActivityViewModel& value) {
      hoverFallback.push_back(value.sourceKey);
    });
    auto hoverModel = model("volume-input", 20);
    hoverModel.hoverChanged = [&hoverReentrant, &clearOnRelease](bool hovered) {
      if (!hovered && clearOnRelease) hoverReentrant.clear();
    };
    assert(hoverReentrant.publish(hoverModel, terminal, now));
    hoverReentrant.setHovered(hoverReentrant.active()->serial, true, now + 50ms);
    clearOnRelease = true;
    hoverReentrant.refreshPresentation(now + 100ms);
    assert(hoverReentrant.active() == nullptr);
    assert(hoverFallback.empty());
  }
}
