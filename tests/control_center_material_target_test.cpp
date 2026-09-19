#include "shell/control_center/shortcut_identity.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"

#include <cassert>
#include <string>
#include <utility>
#include <vector>

int main() {
  std::vector<ShortcutConfig> shortcuts{
      {.type = "wifi"},
      {.type = "wifi", .id = "saved-wifi"},
      {.type = "bluetooth", .id = "saved-wifi"},
  };
  std::size_t next = 0;
  control_center_material::materializeShortcutIds(shortcuts, [&] {
    return "generated-" + std::to_string(++next);
  });
  assert(shortcuts[0].id == std::optional<std::string>{"generated-1"});
  assert(shortcuts[1].id == std::optional<std::string>{"saved-wifi"});
  assert(shortcuts[2].id == std::optional<std::string>{"generated-2"});

  const auto firstTarget = control_center_material::shortcutTargetId(shortcuts[0], 0);
  const auto secondTarget = control_center_material::shortcutTargetId(shortcuts[1], 1);
  std::swap(shortcuts[0], shortcuts[1]);
  assert(control_center_material::shortcutTargetId(shortcuts[1], 1) == firstTarget);
  assert(control_center_material::shortcutTargetId(shortcuts[0], 0) == secondTarget);

  const auto firstPath = control_center_material::shortcutSurfacePath(firstTarget);
  const auto secondPath = control_center_material::shortcutSurfacePath(secondTarget);
  assert(firstPath.back() == firstTarget && secondPath.back() == secondTarget);
  assert(firstPath[firstPath.size() - 2] == "control-center.home.shortcut");

  auto& catalog = Style::MaterialTargetCatalog::instance();
  auto first = catalog.registerInstance(control_center_material::descriptor(
      firstTarget, "Quick Settings: Wi-Fi", "button", firstPath));
  auto second = catalog.registerInstance(control_center_material::descriptor(
      secondTarget, "Quick Settings: Wi-Fi (2)", "button", secondPath));
  assert(first && second && catalog.supported(firstTarget) && catalog.supported(secondTarget));
  first.reset();
  assert(!catalog.supported(firstTarget) && catalog.supported(secondTarget));
  auto reopenedFirst = catalog.registerInstance(control_center_material::descriptor(
      firstTarget, "Quick Settings: Wi-Fi", "button", firstPath));
  assert(reopenedFirst && catalog.supported(firstTarget) && catalog.supported(secondTarget));
  const auto liveFirst = catalog.find(firstTarget);
  const auto liveSecond = catalog.find(secondTarget);
  assert(liveFirst && liveFirst->liveInstances == 1 && liveFirst->descriptor.surfaces == firstPath);
  assert(liveSecond && liveSecond->liveInstances == 1 && liveSecond->descriptor.surfaces == secondPath);
  reopenedFirst.reset();
  second.reset();
  assert(!catalog.supported(firstTarget) && !catalog.supported(secondTarget));

  // A legacy config is usable without a startup write and is distinguished by
  // occurrence until an explicit Settings mutation persists IDs.
  ShortcutConfig legacy{.type = "author/plugin"};
  assert(control_center_material::shortcutTargetId(legacy, 0)
      != control_center_material::shortcutTargetId(legacy, 1));

  ControlCenterConfig config;
  config.shortcuts = shortcuts;
  const auto encoded = noctalia::config::schema::writeTable(
      config, noctalia::config::schema::controlCenterSchema());
  ControlCenterConfig decoded;
  noctalia::config::schema::Diagnostics diagnostics;
  noctalia::config::schema::readInto(
      encoded, decoded, noctalia::config::schema::controlCenterSchema(), "control_center", diagnostics);
  assert(decoded.shortcuts == shortcuts && diagnostics.entries.empty());
}
