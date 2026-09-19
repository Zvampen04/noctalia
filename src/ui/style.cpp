#include "ui/style.h"
#include "render/scene/node.h"
#include "ui/material_resolution_path.h"

#include <algorithm>
#include <utility>

namespace {

  Style::SurfaceMaterialMode g_surfaceMaterial = Style::SurfaceMaterialMode::Flat;
  Style::Metrics g_metrics;
  Style::ControlSettings g_controls;
  Style::MaterialSettings g_materialSettings;
  Style::MaterialOverrides g_materialOverrides;
  Style::CustomEffectResolver g_customEffectResolver;
  noctalia::material::Parameters g_materialParameters;
  float g_cornerRadiusScale = 1.0F;
  bool g_buttonBordersEnabled = true;
  bool g_inputBordersEnabled = true;
  bool g_popupBordersEnabled = true;
  bool g_rtl = false;
  bool g_popupShadowsEnabled = true;
  bool g_cardBordersEnabled = true;

} // namespace

namespace Style {

  const ControlSettings& controls() noexcept { return g_controls; }
  void setControls(const ControlSettings& settings) {
    if (g_controls == settings) return;
    g_controls = settings;
    surfaceMaterialChanged().emit();
  }
  const Metrics& metrics() noexcept { return g_metrics; }
  void setMetrics(const Metrics& values) {
    if (g_metrics == values) return;
    g_metrics = values;
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) member = std::clamp(values.member, low, high);
#include "ui/style_tokens.def"
#undef STYLE_TOKEN
    Node::setDefaultCornerPower(cornerPower);
    surfaceMaterialChanged().emit();
  }

  const noctalia::material::Parameters& materialParameters() noexcept { return g_materialParameters; }
  void setMaterialSettings(const MaterialSettings& settings) {
    if (g_materialSettings == settings) return;
    g_materialSettings = settings;
    g_materialParameters = settings.parameters();
    g_materialParameters.primitive = static_cast<noctalia::material::Primitive>(g_surfaceMaterial);
    surfaceMaterialChanged().emit();
  }

  const MaterialOverrides& materialOverrides() noexcept { return g_materialOverrides; }
  void setMaterialOverrides(const MaterialOverrides& overrides) {
    if (g_materialOverrides == overrides) return;
    g_materialOverrides = overrides;
    surfaceMaterialChanged().emit();
  }
  noctalia::material::Parameters materialFor(
      std::string_view role, std::string_view family, std::string_view surface) noexcept {
    return resolveMaterial(g_materialParameters, g_materialOverrides, role, family, surface);
  }
  noctalia::material::Parameters materialForPath(
      std::string_view role, std::string_view family, std::span<const std::string_view> surfaces) noexcept {
    return resolveMaterialPath(g_materialParameters, g_materialOverrides, role, family, surfaces);
  }
  void setCustomEffectResolver(CustomEffectResolver resolver) {
    g_customEffectResolver = std::move(resolver);
    surfaceMaterialChanged().emit();
  }
  namespace {
    std::optional<CustomEffectBinding> bindCustomEffect(std::optional<ResolvedCustomEffect> resolved) {
      if (!resolved || !g_customEffectResolver) return std::nullopt;
      auto asset = g_customEffectResolver(*resolved);
      if (!asset) return std::nullopt;
      CustomEffectBinding binding;
      binding.asset = std::move(asset);
      for (std::size_t index = 0; index < resolved->parameters.size(); ++index)
        binding.parameters[index / 4][index % 4] = resolved->parameters[index];
      return binding;
    }
  }
  std::optional<CustomEffectBinding> customEffectFor(
      std::string_view role, std::string_view family, std::string_view surface) {
    return bindCustomEffect(resolveCustomEffect(g_materialOverrides, role, family, surface));
  }
  std::optional<CustomEffectBinding> customEffectForPath(
      std::string_view role, std::string_view family, std::span<const std::string_view> surfaces) {
    return bindCustomEffect(resolveCustomEffectPath(g_materialOverrides, role, family, surfaces));
  }

  SurfaceMaterialMode surfaceMaterial() noexcept { return g_surfaceMaterial; }
  bool neumorphicSurfaces() noexcept { return g_surfaceMaterial == SurfaceMaterialMode::Neumorphic; }
  Signal<>& surfaceMaterialChanged() { static Signal<> signal; return signal; }
  void setSurfaceMaterial(SurfaceMaterialMode material) {
    if (g_surfaceMaterial == material) return;
    g_surfaceMaterial = material;
    g_materialParameters.primitive = static_cast<noctalia::material::Primitive>(material);
    surfaceMaterialChanged().emit();
  }

  float cornerRadiusScale() noexcept { return g_cornerRadiusScale; }

  void setCornerRadiusScale(float scale) noexcept {
    scale = std::clamp(scale, 0.0F, 2.0F);
    if (g_cornerRadiusScale == scale) return;
    g_cornerRadiusScale = scale;
    surfaceMaterialChanged().emit();
  }

  bool buttonBordersEnabled() noexcept { return g_buttonBordersEnabled; }

  void setButtonBordersEnabled(bool enabled) {
    if (g_buttonBordersEnabled == enabled) {
      return;
    }
    g_buttonBordersEnabled = enabled;
    buttonBordersChanged().emit();
  }

  Signal<>& buttonBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool inputBordersEnabled() noexcept { return g_inputBordersEnabled; }

  void setInputBordersEnabled(bool enabled) {
    if (g_inputBordersEnabled == enabled) {
      return;
    }
    g_inputBordersEnabled = enabled;
    inputBordersChanged().emit();
  }

  Signal<>& inputBordersChanged() {
    static Signal<> signal;
    return signal;
  }

  bool popupBordersEnabled() noexcept { return g_popupBordersEnabled; }
  void setPopupBordersEnabled(bool enabled) {
    if (g_popupBordersEnabled == enabled) return;
    g_popupBordersEnabled = enabled;
    surfaceMaterialChanged().emit();
  }

  bool rtl() noexcept { return g_rtl; }
  void setRtl(bool rtl) noexcept { g_rtl = rtl; }
  bool cardBordersEnabled() noexcept { return g_cardBordersEnabled; }
  void setCardBordersEnabled(bool enabled) {
    if (g_cardBordersEnabled == enabled) return;
    g_cardBordersEnabled = enabled;
    surfaceMaterialChanged().emit();
  }

  bool popupShadowsEnabled() noexcept { return g_popupShadowsEnabled; }
  void setPopupShadowsEnabled(bool enabled) {
    if (g_popupShadowsEnabled == enabled) return;
    g_popupShadowsEnabled = enabled;
    surfaceMaterialChanged().emit();
  }

  float scaledRadius(float radius, float localScale) noexcept { return radius * localScale * g_cornerRadiusScale; }

  float scaledRadiusSm(float localScale) noexcept { return scaledRadius(radiusSm, localScale); }

  float scaledRadiusMd(float localScale) noexcept { return scaledRadius(radiusMd, localScale); }

  float scaledRadiusLg(float localScale) noexcept { return scaledRadius(radiusLg, localScale); }

  float scaledRadiusXl(float localScale) noexcept { return scaledRadius(radiusXl, localScale); }

} // namespace Style
