#include "ui/material_target_catalog.h"

#include <cassert>
#include <string>

int main() {
  auto& catalog = Style::MaterialTargetCatalog::instance();
  std::size_t changes = 0;
  const auto changed = catalog.changed().connect([&] { ++changes; });
  assert(catalog.supported("popup"));
  assert(catalog.supported("popup.context-menu"));
  assert(catalog.supported("popup.select-dropdown"));
  assert(catalog.supported("popup.dialog"));

  const auto contextPath = Style::materialTargetSurfacePath("settings", "popup.context-menu");
  assert((contextPath == std::vector<std::string>{"settings", "popup", "popup.context-menu"}));
  const auto selectPath = Style::materialTargetSurfacePath("popup", "popup.select-dropdown");
  assert((selectPath == std::vector<std::string>{"popup", "popup.context-menu", "popup.select-dropdown"}));

  const Style::MaterialTargetDescriptor alpha{
      .id = "popup.context-menu.alpha",
      .label = "Alpha context menu",
      .role = "surface",
      .family = "container",
      .surfaces = {"popup", "popup.context-menu", "popup.context-menu.alpha"},
  };
  auto alphaOwner = catalog.registerInstance(alpha);
  assert(alphaOwner);
  assert(catalog.find(alpha.id)->liveInstances == 1);

  auto secondAlphaOwner = catalog.registerInstance(alpha);
  assert(secondAlphaOwner);
  assert(catalog.find(alpha.id)->liveInstances == 2);

  auto movedAlphaOwner = std::move(secondAlphaOwner);
  assert(!secondAlphaOwner);
  movedAlphaOwner.reset();
  assert(catalog.find(alpha.id)->liveInstances == 1);

  auto conflicting = alpha;
  conflicting.family = "panel";
  assert(!catalog.registerInstance(std::move(conflicting)));
  assert(catalog.find(alpha.id)->liveInstances == 1);

  const Style::MaterialTargetDescriptor beta{
      .id = "popup.context-menu.beta",
      .label = "Beta context menu",
      .role = "surface",
      .family = "container",
      .surfaces = {"popup", "popup.context-menu", "popup.context-menu.beta"},
  };
  auto betaOwner = catalog.registerInstance(beta);
  assert(betaOwner);
  assert(Style::materialTargetSurfacePath({}, alpha.id).back() !=
         Style::materialTargetSurfacePath({}, beta.id).back());

  alphaOwner.reset();
  assert(!catalog.supported(alpha.id));
  const auto changesAfterClose = changes;
  auto reopenedAlphaOwner = catalog.registerInstance(alpha);
  assert(reopenedAlphaOwner);
  const auto reopened = catalog.find(alpha.id);
  assert(reopened && reopened->liveInstances == 1 && reopened->descriptor == alpha);
  assert(changes == changesAfterClose + 1);
  reopenedAlphaOwner.reset();
  assert(!catalog.supported(alpha.id));
  betaOwner.reset();
  assert(!catalog.supported(beta.id));

  auto dialogOwner = catalog.registerClassTarget("popup.dialog");
  assert(dialogOwner);
  assert(catalog.find("popup.dialog")->liveInstances == 1);
  dialogOwner.reset();
  assert(catalog.find("popup.dialog")->liveInstances == 0);
  assert(catalog.supported("popup.dialog"));

  return 0;
}
