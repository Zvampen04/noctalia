#pragma once
#include "render/scene/rect_node.h"
#include "ui/style.h"
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <string_view>
#include <string>

// Semantic surface adapter. Supply canonical unlit styles on every sync so
// repeated settings changes never accumulate opacity or relief.
class SurfaceMaterial {
public:
  [[nodiscard]] static RoundedRectStyle styled(
      RectNode& identity, RoundedRectStyle style, float relief,
      MaterialBackdrop backdrop = MaterialBackdrop::Inherited,
      std::string_view family = {}, std::string_view role = "control", std::string_view surface = {},
      std::optional<noctalia::material::Primitive> primitive = std::nullopt) {
    style.material.reset();
    style.materialPlane = false;
    style.customBackground.reset();
    style.relief = 0.0F;
    style.liquidGlass = false;
    if (style.fill.a <= 0.0F || style.outerShadow || style.fillMode == FillMode::None) {
      identity.setMaterialResolver({});
      return style;
    }
    auto resolver = [role = std::string(role), family = std::string(family), surface = std::string(surface),
                     relief, seed = identity.materialSeed(), primitive](std::string_view inheritedSurface) {
      auto parameters = Style::materialFor(role, family, surface.empty() ? inheritedSurface : std::string_view(surface));
      if (primitive) parameters.primitive = *primitive;
      parameters.plateau.elevation = relief < 0.0F
          ? -std::abs(parameters.plateau.elevation) * std::abs(relief)
          : parameters.plateau.elevation * relief;
      parameters.illustration.seed = seed;
      return parameters;
    };
    identity.setMaterialRole(role);
    style.material = resolver(identity.materialSurfaceName());
    style.customBackground = Style::customEffectFor(
        role, family, surface.empty() ? identity.materialSurfaceName() : surface);
    if(style.material->primitive==noctalia::material::Primitive::Plateau) {
      style.fill.a=1.0F;
      for(auto& stop:style.gradientStops)stop.color.a=1.0F;
    }
    identity.setMaterialResolver(std::move(resolver));
    style.materialBackdrop = backdrop;
    style.materialPlane = role == "surface" || role == "overlay";
    return style;
  }

  void sync(Node& owner, RectNode& rectangle, const RoundedRectStyle& style, float relief,
            std::string_view role = "surface", std::string_view family = "container", std::string_view surface = {},
            std::optional<noctalia::material::Primitive> primitive = std::nullopt) {
    auto rendered = styled(rectangle, style, relief, style.materialBackdrop, family, role, surface, primitive);
    rectangle.setStyle(rendered);
    // Explicit material rectangles render their paired shadows in the same GPU
    // pass. This also covers controls using styled() without a SurfaceMaterial
    // owner, and prevents duplicate sibling coatings after live profile changes.
    (void)owner;
  }

  void syncPath(Node& owner, RectNode& rectangle, const RoundedRectStyle& style, float relief,
                std::string_view role, std::string_view family,
                const std::vector<std::string>& surfaces,
                std::optional<noctalia::material::Primitive> primitive = std::nullopt) {
    auto rendered = style;
    rendered.material.reset();
    rendered.materialPlane = false;
    rendered.customBackground.reset();
    rendered.relief = 0.0F;
    rendered.liquidGlass = false;
    if (rendered.fill.a <= 0.0F || rendered.outerShadow || rendered.fillMode == FillMode::None) {
      rectangle.setMaterialResolver({});
      rectangle.setStyle(rendered);
      return;
    }
    auto resolver = [role = std::string(role), family = std::string(family), surfaces,
                     relief, seed = rectangle.materialSeed(), primitive](std::string_view) {
      std::vector<std::string_view> path;
      path.reserve(surfaces.size());
      for (const auto& surface : surfaces) path.push_back(surface);
      auto parameters = Style::materialForPath(role, family, path);
      if (primitive) parameters.primitive = *primitive;
      // A path identifies a concrete nested surface. Optical descendants must remain
      // separate compositor scene planes instead of being folded into an optical ancestor.
      if (parameters.primitive == noctalia::material::Primitive::Optical)
        parameters.optical.planeMode = 1.0F;
      parameters.plateau.elevation = relief < 0.0F
          ? -std::abs(parameters.plateau.elevation) * std::abs(relief)
          : parameters.plateau.elevation * relief;
      parameters.illustration.seed = seed;
      return parameters;
    };
    rectangle.setMaterialRole(role);
    rendered.material = resolver({});
    std::vector<std::string_view> customPath;
    customPath.reserve(surfaces.size());
    for (const auto& surface : surfaces) customPath.push_back(surface);
    rendered.customBackground = Style::customEffectForPath(role, family, customPath);
    if (rendered.material->primitive == noctalia::material::Primitive::Plateau) {
      rendered.fill.a = 1.0F;
      for (auto& stop : rendered.gradientStops) stop.color.a = 1.0F;
    }
    rectangle.setMaterialResolver(std::move(resolver));
    rendered.materialBackdrop = style.materialBackdrop;
    rendered.materialPlane = role == "surface" || role == "overlay";
    rectangle.setStyle(rendered);
    (void)owner;
  }
};
