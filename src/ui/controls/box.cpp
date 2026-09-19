#include "ui/controls/box.h"

#include "render/core/render_styles.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/control_settings_palette.h"

#include <memory>
#include <algorithm>

Box::Box() {
  auto rect = std::make_unique<RectNode>();
  m_rect = static_cast<RectNode*>(addChild(std::move(rect)));
  m_style = m_rect->style();
  m_paletteConn = paletteChanged().connect([this] { applyPalette(); });
  m_materialConn = Style::surfaceMaterialChanged().connect([this] {
    if (m_cardScale) setCardStyle(*m_cardScale, m_cardOpacity, m_cardBorder);
    else syncStyle();
  });
}

const RoundedRectStyle& Box::style() const noexcept { return m_style; }

void Box::setStyle(const RoundedRectStyle& style) {
  m_style = style;
  m_resolveFill = false;
  m_resolveBorder = false;
  m_borderWidth = style.borderWidth;
  syncStyle();
}

void Box::setFill(const ColorSpec& color) {
  m_fill = color;
  m_resolveFill = true;
  applyPalette();
}

void Box::setFill(const Color& color) { setFill(fixedColorSpec(color)); }

void Box::setBorder(const ColorSpec& color, float width) {
  m_border = color;
  m_borderWidth = width;
  m_resolveBorder = true;
  applyPalette();
}

void Box::setBorder(const Color& color, float width) { setBorder(fixedColorSpec(color), width); }

void Box::clearBorder() {
  m_border = clearColorSpec();
  m_borderWidth = 0.0F;
  m_resolveBorder = true;
  applyPalette();
}

void Box::setRadius(float radius) {
  m_style.radius = radius;
  syncStyle();
}

void Box::setRadii(const Radii& radii) {
  m_style.radius = radii;
  syncStyle();
}

void Box::setCornerShapes(const CornerShapes& corners) {
  m_style.corners = corners;
  syncStyle();
}

void Box::setLogicalInset(const RectInsets& inset) {
  m_style.logicalInset = inset;
  syncStyle();
}

void Box::setSegmentContour(const SegmentContour& contour) {
  m_style.segmentContour = contour;
  syncStyle();
}

void Box::setSoftness(float softness) {
  m_style.softness = softness;
  syncStyle();
}

void Box::setNoAa(bool noAa) {
  m_style.noAa = noAa;
  syncStyle();
}

void Box::setMaterialBackdrop(MaterialBackdrop backdrop) {
  m_style.materialBackdrop = backdrop;
  syncStyle();
}

void Box::setMaterialIdentity(std::string_view role, std::string_view family, std::string_view surface) {
  m_cardOwnsFamily = false;
  if (m_materialSurfacePath.empty()
      && m_materialRole == role && m_materialFamily == family && materialSurfaceName() == surface) return;
  m_materialSurfacePath.clear();
  m_materialRole = role;
  m_materialFamily = family;
  setMaterialSurface(surface);
  syncStyle();
}

void Box::setMaterialIdentityPath(
    std::string_view role, std::string_view family, std::vector<std::string> surfaces) {
  m_cardOwnsFamily = false;
  if (m_materialRole == role && m_materialFamily == family && m_materialSurfacePath == surfaces) return;
  m_materialRole = role;
  m_materialFamily = family;
  m_materialSurfacePath = std::move(surfaces);
  setMaterialSurface(m_materialSurfacePath.empty() ? std::string_view{} : m_materialSurfacePath.back());
  syncStyle();
}

void Box::setMaterialPrimitive(std::optional<noctalia::material::Primitive> primitive) {
  if (m_materialPrimitive == primitive) return;
  m_materialPrimitive = primitive;
  syncStyle();
}

void Box::setSurfaceRelief(float relief) {
  m_surfaceRelief = std::clamp(relief, -4.0F, 4.0F);
  syncStyle();
}

void Box::setSize(float w, float h) {
  Node::setSize(w, h);
  m_rect->setFrameSize(w, h);
  syncStyle();
}

void Box::setFrameSize(float w, float h) {
  Node::setFrameSize(w, h);
  m_rect->setFrameSize(w, h);
  syncStyle();
}

void Box::applyPalette() {
  if (m_resolveFill) {
    m_style.fill = resolveColorSpec(m_fill);
    m_style.fillMode = FillMode::Solid;
  }
  if (m_resolveBorder) {
    m_style.border = resolveColorSpec(m_border);
    m_style.borderWidth = m_borderWidth;
  }
  syncStyle();
}

void Box::setFlatStyle() {
  m_surfaceRelief = 0.0F;
  m_fill = colorSpecFromRole(ColorRole::Surface);
  m_border = colorSpecFromRole(ColorRole::Outline);
  m_borderWidth = 0.0F;
  m_resolveFill = true;
  m_resolveBorder = true;
  m_style.fill = resolveColorSpec(m_fill);
  m_style.border = resolveColorSpec(m_border);
  m_style.borderWidth = m_borderWidth;
  m_style.fillMode = FillMode::Solid;
  m_style.corners = {};
  m_style.logicalInset = {};
  m_style.radius = 0;
  m_style.softness = 0;
  syncStyle();
}

void Box::setPanelStyle(bool showBorder) {
  m_surfaceRelief = 0.35F;
  m_fill = colorSpecFromRole(ColorRole::Surface);
  if (showBorder) {
    m_border = colorSpecFromRole(ColorRole::Outline);
    m_borderWidth = Style::borderWidth;
  } else {
    m_border = clearColorSpec();
    m_borderWidth = 0.0F;
  }
  m_resolveFill = true;
  m_resolveBorder = true;
  m_style.fill = resolveColorSpec(m_fill);
  m_style.border = resolveColorSpec(m_border);
  m_style.borderWidth = m_borderWidth;
  m_style.fillMode = FillMode::Solid;
  m_style.corners = {};
  m_style.logicalInset = {};
  m_style.radius = Style::scaledRadiusXl();
  m_style.softness = 1.0F;
  syncStyle();
}

void Box::setDialogStyle() { setPanelStyle(true); }

void Box::setCardStyle(float scale, float fillOpacity, std::optional<bool> showBorder) {
  m_cardScale = scale; m_cardOpacity = fillOpacity; m_cardBorder = showBorder;
  const auto& controls = Style::controls();
  if (controls.card_variant == Style::CardTreatment::Raised) {
    if (m_materialFamily == "container") { m_materialFamily = "card"; m_cardOwnsFamily = true; }
    setSurfaceRelief(controls.card_relief);
    setFill(colorSpecFromRole(controlColorRole(controls.card_face_role), fillOpacity));
    clearBorder();
    setRadius(Style::scaledRadius(controls.card_radius, scale));
    return;
  }
  if (m_cardOwnsFamily) { m_materialFamily = "container"; m_cardOwnsFamily = false; }
  m_surfaceRelief = 0.75F;
  setFill(colorSpecFromRole(ColorRole::SurfaceVariant, fillOpacity));
  if (showBorder.value_or(Style::cardBordersEnabled())) {
    setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
  } else {
    clearBorder();
  }
  setRadius(Style::scaledRadiusXl(scale));
}

void Box::syncStyle() {
  if (m_materialSurfacePath.empty())
    m_material.sync(*this, *m_rect, m_style, m_surfaceRelief, m_materialRole, m_materialFamily, {}, m_materialPrimitive);
  else
    m_material.syncPath(
        *this, *m_rect, m_style, m_surfaceRelief, m_materialRole, m_materialFamily,
        m_materialSurfacePath, m_materialPrimitive);
}
