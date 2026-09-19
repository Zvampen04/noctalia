#include "ui/controls/checkbox.h"

#include "core/input/keybind_matcher.h"
#include "render/scene/input_area.h"
#include "ui/controls/box.h"
#include "ui/controls/glyph.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "ui/control_settings_palette.h"
#include "render/animation/animation_manager.h"

#include <algorithm>
#include <cmath>
#include <memory>

Checkbox::Checkbox() {
  auto box = std::make_unique<Box>();
  m_box = static_cast<Box*>(addChild(std::move(box)));

  m_box->setMaterialIdentity("control", "checkbox-well");
  auto plateau = std::make_unique<Box>();
  m_plateau = static_cast<Box*>(addChild(std::move(plateau)));
  m_plateau->setMaterialIdentity("control", "checkbox-plateau");
  m_plateau->setHitTestVisible(false);
  auto checkGlyph = std::make_unique<Glyph>();
  checkGlyph->setGlyph("check");
  m_checkGlyph = static_cast<Glyph*>(addChild(std::move(checkGlyph)));

  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setOnEnter([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnLeave([this]() { applyState(); });
  area->setOnPress([this](const InputArea::PointerData& /*data*/) { applyState(); });
  area->setOnFocusGain([this]() { applyState(); });
  area->setOnFocusLoss([this]() { applyState(); });
  area->setOnKeyDown([this](const InputArea::KeyData& key) {
    if (!key.pressed || !m_enabled) {
      return;
    }
    if (!KeybindMatcher::matches(KeybindAction::Validate, key.sym, key.modifiers)) {
      return;
    }
    const bool next = !m_checked;
    setChecked(next);
    if (m_onChange) {
      m_onChange(next);
    }
  });
  area->setOnClick([this](const InputArea::PointerData& /*data*/) {
    if (!m_enabled) {
      return;
    }
    const bool next = !m_checked;
    setChecked(next);
    if (m_onChange) {
      m_onChange(next);
    }
  });
  m_inputArea = static_cast<InputArea*>(addChild(std::move(area)));

  applyState();
  m_materialConn = Style::surfaceMaterialChanged().connect([this] { applyState(); markLayoutDirty(); });
  m_paletteConn = paletteChanged().connect([this] { applyState(); });
}

void Checkbox::setChecked(bool checked) {
  if (m_checked == checked) {
    return;
  }
  m_checked = checked;
  if (m_animId && animationManager()) animationManager()->cancel(m_animId);
  m_animId = 0;
  if (animationManager() && Style::controls().checkbox_variant == Style::CheckboxTreatment::Plateau) {
    m_animId = animationManager()->animate(m_checkedProgress, checked ? 1.0F : 0.0F,
        Style::controls().checkbox_transition_ms, Easing::EaseInOutCubic,
        [this](float value) { m_checkedProgress = value; applyState(); }, [this] { m_animId = 0; }, this);
    markPaintDirty();
  } else {
    m_checkedProgress = checked ? 1.0F : 0.0F;
    applyState();
  }
}

void Checkbox::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (m_inputArea != nullptr) {
    m_inputArea->setEnabled(enabled);
  }
  applyState();
}

void Checkbox::setOnChange(std::function<void(bool)> callback) { m_onChange = std::move(callback); }

void Checkbox::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  applyState();
  markLayoutDirty();
}

void Checkbox::setCheckedColors(
    std::optional<ColorSpec> fill, std::optional<ColorSpec> border, std::optional<ColorSpec> glyph
) {
  m_checkedFill = fill;
  m_checkedBorder = border;
  m_checkedGlyph = glyph;
  applyState();
}

bool Checkbox::hovered() const noexcept { return m_inputArea != nullptr && m_inputArea->hovered(); }

bool Checkbox::pressed() const noexcept { return m_inputArea != nullptr && m_inputArea->pressed(); }

void Checkbox::doLayout(Renderer& renderer) {
  const auto& controls = Style::controls();
  const bool plateau = controls.checkbox_variant == Style::CheckboxTreatment::Plateau;
  const float boxSize = (plateau ? controls.checkbox_size : Style::fontSizeTitle + Style::spaceXs) * m_scale;
  const float touchSize = std::max(Style::controlHeightSm * m_scale, boxSize);
  const float boxInset = (touchSize - boxSize) * 0.5F;

  setSize(touchSize, touchSize);

  if (m_box != nullptr) {
    m_box->setPosition(boxInset, boxInset);
    m_box->setFrameSize(boxSize, boxSize);
    m_box->setRadius(plateau ? Style::scaledRadius(controls.checkbox_radius, m_scale) : Style::scaledRadiusSm(m_scale));
    const float centerSize = std::min(boxSize, controls.checkbox_plateau_size * m_scale);
    m_plateau->setFrameSize(centerSize, centerSize);
    m_plateau->setPosition((touchSize - centerSize) * 0.5F, (touchSize - centerSize) * 0.5F);
    m_plateau->setRadius(Style::scaledRadius(controls.checkbox_plateau_radius, m_scale));
  }

  if (m_checkGlyph != nullptr) {
    m_checkGlyph->setGlyphSize((Style::fontSizeBody + Style::spaceXs * 0.5F) * m_scale);
    m_checkGlyph->measure(renderer);
    m_checkGlyph->setPosition(
        std::round(boxInset + (boxSize - m_checkGlyph->width()) * 0.5F),
        std::round(boxInset + (boxSize - m_checkGlyph->height()) * 0.5F)
    );
  }

  if (m_inputArea != nullptr) {
    m_inputArea->setPosition(0.0F, 0.0F);
    m_inputArea->setFrameSize(width(), height());
  }
}

void Checkbox::applyState() {
  if (m_box == nullptr || m_checkGlyph == nullptr) {
    return;
  }

  ColorSpec fill = colorSpecFromRole(ColorRole::Surface);
  ColorSpec border = colorSpecFromRole(ColorRole::Outline);
  ColorSpec glyph = colorSpecFromRole(ColorRole::OnPrimary);
  float borderWidth = Style::borderWidth * m_scale;
  const bool focused = (m_inputArea != nullptr && m_inputArea->focused());
  if (m_checked) {
    fill = m_checkedFill.value_or(colorSpecFromRole(ColorRole::Primary));
    border = m_checkedBorder.value_or(colorSpecFromRole(ColorRole::Primary));
    glyph = m_checkedGlyph.value_or(colorSpecFromRole(ColorRole::OnPrimary));
    if (focused) {
      border = colorSpecFromRole(ColorRole::Secondary);
      borderWidth = Style::emphasizedBorderWidth * m_scale;
    }
  } else if (focused) {
    fill = colorSpecFromRole(ColorRole::Secondary, 0.18F);
    border = colorSpecFromRole(ColorRole::Secondary);
    borderWidth = Style::emphasizedBorderWidth * m_scale;
  } else if (hovered()) {
    border = colorSpecFromRole(ColorRole::Hover);
  }

  const auto& controls = Style::controls();
  const bool plateau = controls.checkbox_variant == Style::CheckboxTreatment::Plateau;
  m_plateau->setVisible(plateau);
  if (plateau) {
    const auto face = colorForRole(controlColorRole(controls.checkbox_face_role));
    const auto accent = colorForRole(controlColorRole(controls.checkbox_accent_role));
    const auto idle = colorForRole(ColorRole::SurfaceVariant);
    m_box->setSurfaceRelief(-1.0F);
    m_box->setFill(lerpColor(idle, m_checkedFill ? resolveColorSpec(*m_checkedFill) : accent, m_checkedProgress));
    m_box->setBorder(focusRingColorSpec(), focused ? Style::focusRingWidth * m_scale : 0.0F);
    m_plateau->setSurfaceRelief(pressed() ? 0.4F : 1.0F);
    m_plateau->setFill(lerpColor(face, colorForRole(ColorRole::OnPrimary), m_checkedProgress));
    m_plateau->clearBorder();
  } else {
    m_box->setSurfaceRelief(m_checked ? -0.4F : 0.4F);
    m_box->setFill(fill);
    m_box->setBorder(border, borderWidth);
  }

  m_checkGlyph->setColor(glyph);
  m_checkGlyph->setVisible(m_checked && (!plateau || controls.checkbox_plateau_tick));

  setOpacity(m_enabled ? 1.0F : 0.55F);
}
