#include "ui/controls/toggle.h"

#include "core/input/keybind_matcher.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/control_settings_palette.h"
#include "ui/surface_material.h"

#include <algorithm>
#include <memory>

Toggle::Toggle() {
  setMaterialIdentity("control", "toggle");
  setSurfaceRelief(-0.8F);
  setAlign(FlexAlign::Center);
  setDirection(FlexDirection::Horizontal);
  setMirrorInRtl(false);
  setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);

  auto well = std::make_unique<RectNode>();
  well->setParticipatesInLayout(false);
  well->setHitTestVisible(false);
  m_well = static_cast<RectNode*>(addChild(std::move(well)));
  auto thumb = std::make_unique<RectNode>();
  m_thumb = static_cast<RectNode*>(addChild(std::move(thumb)));

  auto area = std::make_unique<InputArea>();
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this]() { applyState(); });
  area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (!m_enabled) {
      return;
    }
    activateFromInput();
  });
  area->setFocusable(true);
  area->setOnFocusGain([this]() { applyState(); });
  area->setOnFocusLoss([this]() { applyState(); });
  area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (!key.pressed || !m_enabled) {
      return;
    }
    if (KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
      activateFromInput();
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_inputArea->setParticipatesInLayout(false);
  m_inputArea->setZIndex(1);

  applySize();
  applyState();
  m_materialConn = Style::surfaceMaterialChanged().connect([this] { applySize(); applyAnimatedState(m_animationProgress); });
  m_paletteConn = paletteChanged().connect([this] { applyAnimatedState(m_animationProgress); });
  m_motionConn = MotionService::instance().changed().connect([this] {
    if (!MotionService::instance().enabled()) setCheckedImmediate(m_checked);
  });
}

void Toggle::setChecked(bool checked) {
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;

  const float duration = Style::controls().toggle_variant == Style::ToggleTreatment::Sweep
      ? Style::controls().toggle_transition_ms : static_cast<float>(Style::animFast);
  if (animationManager() != nullptr && MotionService::instance().enabled() && duration > 0) {
    if (m_animId != 0) {
      animationManager()->cancel(m_animId);
    }
    float from = m_animationProgress;
    float to = m_checked ? 1.0F : 0.0F;
    m_animId = animationManager()->animateProgress(
        0.0F, 1.0F, duration,
        [this, from, to](float t) {
          const float native = Style::controls().toggle_variant == Style::ToggleTreatment::Sweep
              ? Style::sweepCurve(t, Style::controls()) : applyEasing(Easing::EaseOutCubic, t);
          const float curve = MotionService::instance().easedProgress(t, native);
          applyAnimatedState(from + (to - from) * curve);
        },
        [this]() { m_animId = 0; }, this
    );
    // Mark dirty so the surface's frame loop restarts and ticks the animation
    markPaintDirty();
  } else {
    if (m_animId != 0 && animationManager() != nullptr) animationManager()->cancel(m_animId);
    m_animId = 0;
    applyState();
  }
}

void Toggle::setCheckedImmediate(bool checked) {
  if (m_checked == checked && m_animId == 0) {
    return;
  }
  m_checked = checked;
  if (m_animId != 0 && animationManager() != nullptr) {
    animationManager()->cancel(m_animId);
    m_animId = 0;
  }
  applyState();
}

void Toggle::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  applyState();
}

void Toggle::setToggleSize(ToggleSize size) {
  if (m_size == size) {
    return;
  }
  m_size = size;
  applySize();
  applyState();
}

void Toggle::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  applySize();
  applyState();
  markLayoutDirty();
}

void Toggle::setOnChange(std::function<void(bool)> callback) { m_onChange = std::move(callback); }

void Toggle::setTabFocusKey(std::string key) {
  if (m_inputArea != nullptr) {
    m_inputArea->setTabFocusKey(std::move(key));
  }
}

void Toggle::activateFromInput() {
  const bool next = !m_checked;
  setChecked(next);
  if (m_onChange) {
    m_onChange(next);
  }
}

bool Toggle::hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }

bool Toggle::pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Toggle::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);

  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0F, 0.0F);
    m_inputArea->setFrameSize(width(), height());
  }
}

LayoutSize Toggle::doMeasure(Renderer& renderer, const LayoutConstraints& constraints) {
  return measureByLayout(renderer, constraints);
}

void Toggle::doArrange(Renderer& renderer, const LayoutRect& rect) { arrangeByLayout(renderer, rect); }

void Toggle::applySize() {
  const auto& controls = Style::controls();
  if (controls.toggle_variant == Style::ToggleTreatment::Sweep) {
    const float height = controls.toggle_height * m_scale;
    const float width = height * controls.toggle_width_ratio;
    m_thumb->setParticipatesInLayout(false);
    m_well->setVisible(true);
    setClipChildren(true);
    setMinWidth(width); setMaxWidth(width);
    setMinHeight(height); setMaxHeight(height);
    setSizeFromLayout(width, height);
    setPadding(0);
    setRadius(Style::scaledRadius(controls.toggle_radius, m_scale));
    m_well->setFrameSize(width, height);
    return;
  }
  m_well->setVisible(false);
  m_thumb->setParticipatesInLayout(true);
  setClipChildren(false);
  setMinWidth(0); setMaxWidth(0); setMinHeight(0); setMaxHeight(0);

  switch (m_size) {
  case ToggleSize::Small:
    m_thumbSize = Style::toggleThumbSizeSm * m_scale;
    m_inset = Style::toggleInsetSm * m_scale;
    m_travel = Style::toggleTravelSm * m_scale;
    break;
  case ToggleSize::Medium:
    m_thumbSize = Style::toggleThumbSizeMd * m_scale;
    m_inset = Style::toggleInsetMd * m_scale;
    m_travel = Style::toggleTravelMd * m_scale;
    break;
  case ToggleSize::Large:
    m_thumbSize = Style::toggleThumbSizeLg * m_scale;
    m_inset = Style::toggleInsetLg * m_scale;
    m_travel = Style::toggleTravelLg * m_scale;
    break;
  }

  m_thumb->setFrameSize(m_thumbSize, m_thumbSize);
  setRadius(Style::scaledRadius((m_thumbSize + (m_inset * 2.0F)) * 0.5F));
}

void Toggle::applyState() {
  // Hover, focus and live metric changes must not jump an active sweep to its endpoint.
  applyAnimatedState(m_animId != 0 ? m_animationProgress : (m_checked ? 1.0F : 0.0F));
}

void Toggle::applyAnimatedState(float t) {
  m_animationProgress = t;
  const auto& controls = Style::controls();
  if (controls.toggle_variant == Style::ToggleTreatment::Sweep) {
    const auto geometry = Style::sweepGeometry(controls, m_scale, t, Style::rtl());
    setSurfaceRelief(1.0F);
    setFill(colorSpecFromRole(controlColorRole(controls.toggle_face_role)));
    setBorder(focusRingColorSpec(), m_inputArea && m_inputArea->focused() ? Style::focusRingWidth : 0.0F);
    RoundedRectStyle well;
    well.fill = colorForRole(controlColorRole(controls.toggle_well_role));
    well.radius = Style::scaledRadius(controls.toggle_radius, m_scale);
    m_well->setStyle(SurfaceMaterial::styled(*m_well, well, -1.0F, MaterialBackdrop::Inherited, "toggle-well"));
    auto indicator = well;
    indicator.paintClip = RoundedPaintClip{-geometry.x, -geometry.y, width(), height(),
        Style::scaledRadius(controls.toggle_radius, m_scale), cornerPower()};
    indicator.fill = colorForRole(controlColorRole(controls.toggle_face_role));
    m_thumb->setStyle(SurfaceMaterial::styled(*m_thumb, indicator, 1.0F, MaterialBackdrop::Inherited, "toggle-indicator"));
    m_thumb->setFrameSize(geometry.indicatorWidth, geometry.indicatorHeight);
    m_thumb->setPosition(geometry.x, geometry.y);
    setOpacity(m_enabled ? 1.0F : 0.55F);
    return;
  }
  setSurfaceRelief(-0.8F);
  const Color trackColor = lerpColor(colorForRole(ColorRole::Outline), colorForRole(ColorRole::Primary), t);
  const Color thumbColor = lerpColor(colorForRole(ColorRole::OnPrimary), colorForRole(ColorRole::OnPrimary), t);
  const float thumbX = m_inset + m_travel * (Style::rtl() ? 1.0F - t : t);
  ColorSpec borderColor = colorSpecFromRole(ColorRole::Outline);

  if (m_enabled) {
    if (m_inputArea != nullptr && m_inputArea->focused()) {
      borderColor = focusRingColorSpec();
    } else if (hovered()) {
      borderColor = colorSpecFromRole(ColorRole::Hover);
    } else if (m_checked) {
      borderColor = colorSpecFromRole(ColorRole::Primary);
    }
  }

  setFill(trackColor);
  setBorder(borderColor, m_inputArea != nullptr && m_inputArea->focused() ? Style::focusRingWidth : Style::materialFor("control", "toggle", materialSurfaceName()).primitive == noctalia::material::Primitive::Plateau ? 0.0F : Style::borderWidth);
  m_thumb->setPosition(thumbX, m_inset);

  auto thumbStyle = m_thumb->style();
  thumbStyle.paintClip.reset();
  thumbStyle.fillMode = FillMode::Solid;
  thumbStyle.radius = Style::scaledRadius(m_thumbSize * 0.5F);
  thumbStyle.cornerPower = 2.0F;
  thumbStyle.softness = 1.0F;
  thumbStyle.borderWidth = 0.0F;
  thumbStyle.fill = thumbColor;
  m_thumb->setStyle(SurfaceMaterial::styled(*m_thumb, thumbStyle, 0.8F, MaterialBackdrop::Inherited, "toggle"));

  // Padding keeps Flex size consistent regardless of thumb position
  const float rightPad = m_inset + m_travel - (thumbX - m_inset);
  setPadding(m_inset, rightPad, m_inset, thumbX);

  if (m_enabled) {
    setOpacity(1.0F);
  } else {
    setOpacity(0.55F);
  }
}
