#pragma once
#define NOCTALIA_HAS_SURFACE_MATERIALS 1

#include "ui/signal.h"
#include "ui/control_settings.h"
#include "material/material.h"
#include "ui/material_overrides.h"
#include "render/custom_effect/custom_effect_types.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace Style {

  // Values update with the active profile. Keep consumers live: do not capture
  // these in namespace-scope constants or static geometry caches.
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) inline type member = initial;
#include "ui/style_tokens.def"
#undef STYLE_TOKEN

  struct Metrics {
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) type member = initial;
#include "ui/style_tokens.def"
#undef STYLE_TOKEN
    bool operator==(const Metrics&) const = default;
  };
  [[nodiscard]] const Metrics& metrics() noexcept;
  void setMetrics(const Metrics& values);

  [[nodiscard]] const ControlSettings& controls() noexcept;
  void setControls(const ControlSettings& settings);

  struct MaterialSettings {
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) float key = initial;
#include "material/fields.def"
#undef MATERIAL_FIELD
    [[nodiscard]] noctalia::material::Parameters parameters() const noexcept {
      noctalia::material::Parameters result;
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) result.member = key;
#include "material/fields.def"
#undef MATERIAL_FIELD
      return noctalia::material::sanitize(result);
    }
    bool operator==(const MaterialSettings&) const = default;
  };
  [[nodiscard]] const noctalia::material::Parameters& materialParameters() noexcept;
  void setMaterialSettings(const MaterialSettings& settings);
  [[nodiscard]] const MaterialOverrides& materialOverrides() noexcept;
  void setMaterialOverrides(const MaterialOverrides& overrides);
  [[nodiscard]] noctalia::material::Parameters materialFor(
      std::string_view role = {}, std::string_view family = {}, std::string_view surface = {}) noexcept;
  [[nodiscard]] noctalia::material::Parameters materialForPath(
      std::string_view role, std::string_view family, std::span<const std::string_view> surfaces) noexcept;
  using CustomEffectResolver = std::function<std::shared_ptr<const CustomEffectAsset>(const ResolvedCustomEffect&)>;
  void setCustomEffectResolver(CustomEffectResolver resolver);
  [[nodiscard]] std::optional<CustomEffectBinding> customEffectFor(
      std::string_view role = {}, std::string_view family = {}, std::string_view surface = {});
  [[nodiscard]] std::optional<CustomEffectBinding> customEffectForPath(
      std::string_view role, std::string_view family, std::span<const std::string_view> surfaces);

  enum class SurfaceMaterialMode { Flat, Neumorphic, LiquidGlass, Illustrated };
  [[nodiscard]] SurfaceMaterialMode surfaceMaterial() noexcept;
  [[nodiscard]] bool neumorphicSurfaces() noexcept;
  void setSurfaceMaterial(SurfaceMaterialMode material);
  Signal<>& surfaceMaterialChanged();

  [[nodiscard]] float cornerRadiusScale() noexcept;
  void setCornerRadiusScale(float scale) noexcept;

  [[nodiscard]] bool buttonBordersEnabled() noexcept;
  void setButtonBordersEnabled(bool enabled);
  Signal<>& buttonBordersChanged();

  [[nodiscard]] bool inputBordersEnabled() noexcept;
  void setInputBordersEnabled(bool enabled);
  Signal<>& inputBordersChanged();

  [[nodiscard]] bool popupBordersEnabled() noexcept;
  void setPopupBordersEnabled(bool enabled);

  [[nodiscard]] bool rtl() noexcept;
  void setRtl(bool rtl) noexcept;

  [[nodiscard]] bool cardBordersEnabled() noexcept;
  void setCardBordersEnabled(bool enabled);

  [[nodiscard]] bool popupShadowsEnabled() noexcept;
  void setPopupShadowsEnabled(bool enabled);

  [[nodiscard]] float scaledRadius(float radius, float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusSm(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusMd(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusLg(float localScale = 1.0F) noexcept;
  [[nodiscard]] float scaledRadiusXl(float localScale = 1.0F) noexcept;

} // namespace Style
