#include "material/material.h"
#include "shell/bar/bar_material_target.h"
#include "ui/material_resolution_path.h"

#include <array>
#include <cassert>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

int main() {
  using noctalia::material::Primitive;

  assert(noctalia::bar::barMaterialTarget("default") == "bar.instance.default");
  assert(noctalia::bar::barSectionMaterialTarget("default", "status") == "bar.section.default.status");
  assert(noctalia::bar::barWidgetMaterialTarget("meter-a") == "bar.widget.meter-a");
  const auto fallback = noctalia::bar::barWidgetMaterialTarget("an invalid/widget target");
  assert(fallback.starts_with("bar.widget.id-") && fallback.size() == 30);
  assert(fallback == noctalia::bar::barWidgetMaterialTarget("an invalid/widget target"));
  assert(noctalia::bar::makeBarWidgetPlacementToken("meter-a") == "@widget:meter-a");
  assert(noctalia::bar::barWidgetPlacementTokenId("@widget:meter-a") == "meter-a");
  assert(noctalia::bar::barWidgetPlacementTokenId("meter-a").empty());
  const std::unordered_map<std::string, std::string> placements{{"meter-a", "cpu"}, {"meter-b", "cpu"}};
  const auto resolvedA = noctalia::bar::resolveBarWidgetLaneEntry("@widget:meter-a", placements);
  const auto resolvedB = noctalia::bar::resolveBarWidgetLaneEntry("@widget:meter-b", placements);
  assert(resolvedA.widgetConfigName == "cpu" && resolvedA.placementId == "meter-a");
  assert(resolvedB.widgetConfigName == "cpu" && resolvedB.placementId == "meter-b");
  const auto legalLiteral = noctalia::bar::resolveBarWidgetLaneEntry("@widget:literal-module", placements);
  assert(legalLiteral.widgetConfigName == "@widget:literal-module" && !legalLiteral.isPlacement());
  const auto ordinaryLiteral = noctalia::bar::resolveBarWidgetLaneEntry("cpu", placements);
  assert(ordinaryLiteral.widgetConfigName == "cpu" && !ordinaryLiteral.isPlacement());
  const auto legacy = noctalia::bar::legacyBarWidgetPlacementId("default", "status", "cpu", 0, {});
  assert(legacy == noctalia::bar::legacyBarWidgetPlacementId("default", "status", "cpu", 0, {}));
  const std::unordered_set<std::string> collision{legacy};
  assert(noctalia::bar::legacyBarWidgetPlacementId("default", "status", "cpu", 0, collision) == legacy + "-2");

  Style::MaterialOverrides overrides;
  overrides.surfaces["bar"].primitive = Primitive::Flat;
  overrides.surfaces["bar.instance.default"].tint_opacity = 0.2F;
  overrides.surfaces["bar.section.default.status"].rim = 0.35F;
  // Both placements may point at the same widget type/config; their persistent placement IDs
  // retain independent material ownership.
  overrides.surfaces["bar.widget.meter-a"].primitive = Primitive::Optical;
  overrides.surfaces["bar.widget.meter-a"].rim = 0.8F;
  overrides.surfaces["bar.widget.meter-a"].lens_strength = 0.7F;
  overrides.surfaces["bar.widget.meter-b"].primitive = Primitive::Plateau;

  const std::array<std::string_view, 4> cpuPath{
      "bar", "bar.instance.default", "bar.section.default.status", "bar.widget.meter-a"};
  const std::array<std::string_view, 4> ramPath{
      "bar", "bar.instance.default", "bar.section.default.status", "bar.widget.meter-b"};
  const auto cpu = Style::resolveMaterialPath({}, overrides, "surface", "bar-widget", cpuPath);
  const auto ram = Style::resolveMaterialPath({}, overrides, "surface", "bar-widget", ramPath);

  assert(cpu.primitive == Primitive::Optical);
  assert(cpu.optical.tintOpacity == 0.2F);
  assert(cpu.optical.rim == 0.8F);
  assert(cpu.optical.lensStrength == 0.7F);
  assert(ram.primitive == Primitive::Plateau);
  assert(ram.optical.rim == 0.35F);
  assert(ram.optical.tintOpacity == 0.2F);

  overrides.families["bar-widget"].customBackground = "user.shared-sheen";
  overrides.families["bar-widget"].customBackgroundDigest = std::string(64, 'a');
  overrides.families["bar-widget"].customSampleRadiusPx = 12.0F;
  overrides.families["bar-widget"].customParameters[0] = 0.25F;
  overrides.surfaces["bar.widget.meter-a"].customParameters[0] = 0.75F;
  overrides.surfaces["bar.widget.meter-a"].customParameters[31] = 100.0F;
  const auto cpuEffect = Style::resolveCustomEffectPath(overrides, "surface", "bar-widget", cpuPath);
  const auto ramEffect = Style::resolveCustomEffectPath(overrides, "surface", "bar-widget", ramPath);
  assert(cpuEffect && ramEffect);
  assert(cpuEffect->stableId == "user.shared-sheen" && cpuEffect->maxSampleRadiusPx == 12.0F);
  assert(cpuEffect->parameters[0] == 0.75F);
  assert(cpuEffect->parameters[31] == Style::kCustomEffectParameterMax);
  assert(ramEffect->parameters[0] == 0.25F);

  // Lane order is not part of either target, so reorder preserves both results.
  const std::array reordered{ramPath, cpuPath};
  assert(Style::resolveMaterialPath({}, overrides, "surface", "bar-widget", reordered[0]).primitive
      == Primitive::Plateau);
  assert(Style::resolveMaterialPath({}, overrides, "surface", "bar-widget", reordered[1]).primitive
      == Primitive::Optical);

  // Reset removes only the most-specific target and exposes inherited bar/section fields.
  overrides.surfaces.erase("bar.widget.meter-a");
  const auto reset = Style::resolveMaterialPath({}, overrides, "surface", "bar-widget", cpuPath);
  assert(reset.primitive == Primitive::Flat);
  assert(reset.optical.rim == 0.35F && reset.optical.tintOpacity == 0.2F);
  const auto resetEffect = Style::resolveCustomEffectPath(overrides, "surface", "bar-widget", cpuPath);
  assert(resetEffect && resetEffect->parameters[0] == 0.25F);
}
