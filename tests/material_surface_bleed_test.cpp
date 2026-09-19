#include "shell/surface/shadow.h"
#include "ui/popup_chrome.h"
#include "ui/style.h"
#include "wayland/popup_surface.h"
#include <algorithm>
#include <cassert>

int main() {
  const ShellConfig::ShadowConfig conventional{};
  Style::setSurfaceMaterial(Style::SurfaceMaterialMode::Flat);
  assert(shell::surface_shadow::bleed(false, conventional).left == 0);
  const auto drop = shell::surface_shadow::bleed(true, conventional);
  assert(drop.left >= shell::surface_shadow::kBlurRadius);
  Style::setSurfaceMaterial(Style::SurfaceMaterialMode::Neumorphic);
  Style::MaterialSettings material;
  material.contact_radius = 60;
  material.shadow_distance = 20;
  material.contact_strength = 1;
  Style::setMaterialSettings(material);
  const auto paired = shell::surface_shadow::bleed(false, conventional);
  assert(paired.left == 112 && paired.right == 112 && paired.up == 112 && paired.down == 112);
  const auto popup = popup_chrome::computeGeometry(240, 120, conventional, false);
  assert(popup.surfaceWidth == 464 && popup.surfaceHeight == 344);
  const auto input = popup.inputRect();
  assert(input.x == 112 && input.y == 112 && input.width == 240 && input.height == 120);
  const auto rounded = popup_chrome::roundedContentRegion(popup, 24.0F, 2.0F);
  assert(!rounded.empty());
  assert(rounded.front().x > input.x && rounded.front().width < input.width);
  assert(rounded.back().x > input.x && rounded.back().width < input.width);
  assert(std::ranges::any_of(rounded, [&](const InputRect& strip) {
    return strip.x == input.x && strip.width == input.width;
  }));

  auto scaled = popup;
  scaled.contentWidth = 480.0F;
  scaled.contentHeight = 240.0F;
  const auto scaledRounded = popup_chrome::roundedContentRegion(scaled, 48.0F, 4.0F);
  assert(!scaledRounded.empty() && scaledRounded.front().x > input.x);
  assert(scaledRounded.front().width < 480);

  auto tiny = popup;
  tiny.contentWidth = 3.0F;
  tiny.contentHeight = 2.0F;
  const auto clamped = popup_chrome::roundedContentRegion(tiny, 100.0F, 10.0F);
  assert(!clamped.empty());
  assert(std::ranges::all_of(clamped, [&](const InputRect& strip) {
    return strip.x >= input.x && strip.y >= input.y && strip.width > 0 && strip.height > 0
        && strip.x + strip.width <= input.x + 3 && strip.y + strip.height <= input.y + 2;
  }));

  auto empty = popup;
  empty.contentWidth = 0.0F;
  empty.contentHeight = 0.0F;
  assert(popup_chrome::roundedContentRegion(empty, 20.0F, 2.0F).empty());
  Style::MaterialOverrides scopes;
  scopes.surfaces["settings"].contact_radius = 0;
  scopes.surfaces["settings"].shadow_distance = 0;
  Style::setMaterialOverrides(scopes);
  assert(popup_chrome::computeGeometry(240, 120, conventional, false, "settings").bleed.left == 2);
  assert(popup_chrome::computeGeometry(240, 120, conventional, false, "tray").bleed.left == 112);
  scopes.families["card"].contact_radius = 20;
  scopes.families["card"].shadow_distance = 4;
  Style::setMaterialOverrides(scopes);
  assert(popup_chrome::computeGeometry(240, 120, conventional, false, "tray", "card").bleed.left == 36);
  assert(popup_chrome::computeGeometry(240, 120, conventional, false, "settings", "card").bleed.left == 2);
  scopes.surfaces["settings"].elevation = -3;
  Style::setMaterialOverrides(scopes);
  assert(popup_chrome::computeGeometry(240, 120, conventional, false, "settings").bleed.left == 0);
  scopes.surfaces["settings"].elevation = 3;
  scopes.surfaces["settings"].contact_strength = 0;
  Style::setMaterialOverrides(scopes);
  assert(popup_chrome::computeGeometry(240, 120, conventional, false, "settings").bleed.left == 0);
}
