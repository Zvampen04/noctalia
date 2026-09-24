#include "ui/controls/segmented.h"

#include "render/core/render_styles.h"
#include "render/animation/animation_manager.h"
#include "render/animation/motion_service.h"
#include "ui/controls/box.h"
#include "ui/control_settings_palette.h"
#include "render/scene/input_area.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/separator.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <memory>
#include <utility>

Segmented::Segmented() {
  setMaterialIdentity("control", "segmented");
  setDirection(FlexDirection::Horizontal);
  setAlign(FlexAlign::Stretch);
  setGap(0.0F);
  applyOuterStyle();

  auto indicator = std::make_unique<Box>();
  indicator->setMaterialIdentity("control", "segmented-indicator");
  indicator->setParticipatesInLayout(false);
  indicator->setHitTestVisible(false);
  indicator->setVisible(false);
  indicator->setZIndex(0);
  m_indicator = static_cast<Box*>(addChild(std::move(indicator)));

  auto area = std::make_unique<InputArea>();
  area->setFocusable(true);
  area->setHitTestVisible(false);
  m_focusArea = static_cast<InputArea*>(addChild(std::move(area)));
  m_focusArea->setParticipatesInLayout(false);
  m_focusArea->setZIndex(2);

  m_rovingNav.setOptions(
      RovingListNavController::Options{
          .axis = RovingListNavAxis::Horizontal,
          .mode = RovingListNavMode::FollowFocus,
          .scrollIntoView = {},
          .syncIndexFromSelection = [this]() { return m_selected; },
      }
  );
  m_rovingNav.bindFocusArea(m_focusArea);
  m_presentationConn = Style::surfaceMaterialChanged().connect([this] { refreshPresentation(); });
  m_paletteConn = paletteChanged().connect([this] { refreshVariants(); paintIndicator(); });
  m_motionConn = MotionService::instance().changed().connect([this] {
    if (!MotionService::instance().enabled()) updateIndicator(false);
  });
}

Segmented::~Segmented() { cancelIndicator(); }


std::size_t Segmented::addOption(std::string_view label) { return addOption(label, std::string_view{}); }

std::size_t Segmented::addOption(std::string_view label, std::string_view glyph) {
  const std::size_t index = m_buttons.size();
  if (index > 0) {
    auto sep = makeSegmentSeparator();
    m_separators.push_back(sep.get());
    addChild(std::move(sep));
  }
  auto btn = makeSegmentButton(label, glyph, index);
  btn->setZIndex(1);
  Button* raw = btn.get();
  m_buttons.push_back(raw);
  m_rovingNav.registerItem(raw, [this, index]() { setSelectedIndex(index); });
  // Establish the new option before attachment so its initial palette does not animate.
  refreshVariants();
  addChild(std::move(btn));
  m_rovingNav.notifyExternalSelectionChanged();
  return index;
}

void Segmented::setSelectedIndex(std::size_t index) {
  if (index >= m_buttons.size() || index == m_selected) {
    return;
  }
  m_selected = index;
  refreshVariants();
  updateIndicator(true);
  m_rovingNav.notifyExternalSelectionChanged();
  if (m_onChange) {
    m_onChange(index);
  }
}

void Segmented::setFontSize(float size) {
  m_fontSize = size;
  const float fs = effectiveFontSize();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      btn->setFontSize(fs);
      btn->setGlyphSize(fs);
    }
  }
}

void Segmented::setScale(float scale) {
  m_scale = std::max(0.1F, scale);
  applyOuterStyle();
  const float fs = effectiveFontSize();
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      applyButtonMetrics(*btn);
      btn->setFontSize(fs);
      btn->setGlyphSize(fs);
    }
  }
  const float ruleW = std::max(1.0F, Style::borderWidth * m_scale);
  for (Separator* sep : m_separators) {
    if (sep != nullptr) {
      sep->setThickness(ruleW);
    }
  }
  refreshVariants();
  markLayoutDirty();
}

void Segmented::setCompact(bool compact) {
  if (m_compact == compact) {
    return;
  }
  m_compact = compact;
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      applyButtonMetrics(*btn);
    }
  }
  markLayoutDirty();
}

void Segmented::setPadding(float padding) {
  m_outerPadding = padding;
  Flex::setPadding(padding);
}

void Segmented::setPadding(float vertical, float horizontal) {
  m_outerPadding = vertical;
  Flex::setPadding(vertical, horizontal);
}

void Segmented::setPadding(float top, float right, float bottom, float left) {
  m_outerPadding = top;
  Flex::setPadding(top, right, bottom, left);
}

void Segmented::setOptionTooltip(std::size_t index, std::string_view text) {
  if (index < m_buttons.size() && m_buttons[index] != nullptr) {
    m_buttons[index]->setTooltip(text);
  }
}

void Segmented::clearOptions() {
  cancelIndicator();
  m_indicatorMotion.clear();
  m_indicator->setVisible(false);
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      (void)removeChild(btn);
    }
  }
  for (Separator* sep : m_separators) {
    if (sep != nullptr) {
      (void)removeChild(sep);
    }
  }
  m_buttons.clear();
  m_separators.clear();
  m_rovingNav.clearItems();
  m_selected = 0;
  markLayoutDirty();
}

void Segmented::setOnChange(std::function<void(std::size_t)> callback) { m_onChange = std::move(callback); }

void Segmented::setSurfaceOpacity(float opacity) {
  const float clamped = std::clamp(opacity, 0.0F, 1.0F);
  if (m_surfaceOpacity == clamped) {
    return;
  }
  m_surfaceOpacity = clamped;
  applyOuterStyle();
}

void Segmented::setSurfaceRole(ColorRole role) {
  if (m_surfaceRole == role) {
    return;
  }
  m_surfaceRole = role;
  applyOuterStyle();
}

void Segmented::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  m_focusArea->setEnabled(enabled);
  for (Button* btn : m_buttons) {
    if (btn != nullptr) {
      btn->setEnabled(enabled);
    }
  }
  setOpacity(enabled ? 1.0F : 0.55F);
}

std::unique_ptr<Separator> Segmented::makeSegmentSeparator() {
  auto sep = std::make_unique<Separator>();
  sep->setOrientation(SeparatorOrientation::VerticalRule);
  sep->setThickness(std::max(1.0F, Style::borderWidth * m_scale));
  sep->setColor(colorSpecFromRole(ColorRole::Outline));
  sep->setFlexGrow(0.0F);
  return sep;
}

std::unique_ptr<Button>
Segmented::makeSegmentButton(std::string_view label, std::string_view glyph, std::size_t index) {
  auto btn = std::make_unique<Button>();
  if (!glyph.empty()) {
    btn->setGlyph(glyph);
    btn->setGlyphSize(effectiveFontSize());
  }
  if (!label.empty()) {
    btn->setText(label);
    btn->setFontSize(effectiveFontSize());
  }
  applyButtonMetrics(*btn);
  btn->setOnClick([this, index]() { setSelectedIndex(index); });
  btn->setTabStop(false);
  btn->setFlexGrow(m_equalSegmentWidths ? 1.0F : 0.0F);
  btn->setContentAlign(ButtonContentAlign::Center);
  btn->setEnabled(m_enabled);
  return btn;
}

void Segmented::applyButtonMetrics(Button& button) const {
  if (m_compact) {
    button.setMinHeight(Style::controlHeightSm * m_scale);
    button.setPadding(Style::spaceXs * m_scale, Style::spaceSm * m_scale);
    return;
  }

  button.setMinHeight(Style::controlHeight * m_scale);
  button.setPadding(Style::spaceXs * m_scale, Style::spaceMd * m_scale);
}

void Segmented::setEqualSegmentWidths(bool equalWidths) {
  if (m_equalSegmentWidths == equalWidths) {
    return;
  }
  m_equalSegmentWidths = equalWidths;
  for (Button* b : m_buttons) {
    if (b != nullptr) {
      b->setFlexGrow(m_equalSegmentWidths ? 1.0F : 0.0F);
    }
  }
  markLayoutDirty();
}

void Segmented::refreshVariants() {
  const std::size_t n = m_buttons.size();
  const bool floating = Style::controls().segmented_variant == Style::SegmentedTreatment::Floating;
  const float r = floating ? Style::scaledRadius(Style::controls().segmented_indicator_radius, m_scale) : Style::scaledRadiusMd(m_scale);
  for (Separator* separator : m_separators) separator->setVisible(!floating);
  for (std::size_t i = 0; i < n; ++i) {
    if (m_buttons[i] == nullptr) {
      continue;
    }
    if (floating) {
      m_buttons[i]->setVariant(ButtonVariant::Ghost);
      auto palette = Button::defaultPalette(ButtonVariant::Ghost);
      palette.normal.bg = palette.hover.bg = palette.pressed.bg = palette.disabled.bg = clearColorSpec();
      palette.normal.border = palette.hover.border = palette.pressed.border = palette.disabled.border = clearColorSpec();
      const auto selectedRole = Style::controls().segmented_indicator_opacity > 0
          ? controlForegroundRole(Style::controls().segmented_indicator_role) : ColorRole::Primary;
      const auto textRole = i == m_selected ? selectedRole : ColorRole::OnSurface;
      palette.normal.label = colorSpecFromRole(textRole);
      palette.hover.label = palette.pressed.label = colorSpecFromRole(i == m_selected ? textRole : ColorRole::Primary);
      palette.disabled.label = colorSpecFromRole(textRole, 0.55F);
      palette.selected.reset();
      m_buttons[i]->setCustomPalette(palette);
      m_buttons[i]->setRadii(Radii{r});
      continue;
    }
    const bool selected = i == m_selected;
    m_buttons[i]->setVariant(selected ? ButtonVariant::TabActive : ButtonVariant::Tab);
    if (Style::neumorphicSurfaces()) {
      auto palette = Button::defaultPalette(selected ? ButtonVariant::TabActive : ButtonVariant::Tab);
      if (!selected) {
        // The group is the shared material surface. Keep unselected options
        // transparent in every state so their individual halos cannot form seams.
        palette.normal.bg = palette.hover.bg = palette.pressed.bg = palette.disabled.bg = clearColorSpec();
        palette.hover.label = palette.pressed.label = colorSpecFromRole(ColorRole::Primary);
      }
      m_buttons[i]->setCustomPalette(palette);
    }
    Radii radii;
    if (n == 1) {
      radii = Radii{r, r, r, r};
    } else if (i == 0) {
      radii = Radii{r, 0.0F, 0.0F, r};
    } else if (i == n - 1) {
      radii = Radii{0.0F, r, r, 0.0F};
    } else {
      radii = Radii{0.0F};
    }
    m_buttons[i]->setRadii(radii);
  }
}

void Segmented::applyOuterStyle() {
  Flex::setPadding(m_outerPadding);
  setFill(colorSpecFromRole(m_surfaceRole, m_surfaceOpacity));
  clearBorder();
  setRadius(Style::controls().segmented_variant == Style::SegmentedTreatment::Floating
      ? Style::scaledRadius(Style::controls().segmented_indicator_radius, m_scale) : Style::scaledRadiusMd(m_scale));
}

void Segmented::doLayout(Renderer& renderer) {
  Flex::doLayout(renderer);
  m_rovingNav.layoutOverlay(width(), height());
  updateIndicator(false);
}

float Segmented::effectiveFontSize() const noexcept {
  return (m_fontSize > 0.0F ? m_fontSize : Style::fontSizeBody) * m_scale;
}

void Segmented::cancelIndicator() {
  if (m_indicatorAnimation && animationManager()) animationManager()->cancel(m_indicatorAnimation);
  m_indicatorAnimation = 0;
}

void Segmented::paintIndicator() {
  if (!m_indicator) return;
  const auto& controls = Style::controls();
  const bool visible = controls.segmented_variant == Style::SegmentedTreatment::Floating && m_indicatorMotion.valid();
  m_indicator->setVisible(visible);
  if (!visible) return;
  const auto& frame = m_indicatorMotion.current();
  m_indicator->setPosition(frame.x, frame.y);
  m_indicator->setFrameSize(frame.width, frame.height);
  m_indicator->setRadius(Style::scaledRadius(controls.segmented_indicator_radius, m_scale));
  m_indicator->setFill(colorSpecFromRole(controlColorRole(controls.segmented_indicator_role)));
  m_indicator->setOpacity(controls.segmented_indicator_opacity);
  m_indicator->clearBorder();
}

void Segmented::updateIndicator(bool animate) {
  const auto& controls = Style::controls();
  if (controls.segmented_variant != Style::SegmentedTreatment::Floating || m_buttons.empty() || m_selected >= m_buttons.size()) {
    cancelIndicator(); m_indicatorMotion.clear(); paintIndicator(); return;
  }
  const auto* button = m_buttons[m_selected];
  if (button->width() <= 0 || button->height() <= 0 || width() <= 0 || height() <= 0) {
    cancelIndicator(); m_indicatorMotion.clear(); paintIndicator(); return;
  }
  const auto target = segmentedIndicatorTarget({button->x(), button->y(), button->width(), button->height()},
      controls.segmented_indicator_inset * m_scale, width(), height());
  // A layout pass during travel must not restart or finish an unchanged trip.
  if (!animate && m_indicatorAnimation && target == m_indicatorMotion.target() && MotionService::instance().enabled() && controls.segmented_transition_ms > 0) {
    paintIndicator(); return;
  }
  cancelIndicator();
  if (!animate || !animationManager() || !MotionService::instance().enabled() || controls.segmented_transition_ms <= 0) {
    m_indicatorMotion.snap(target); paintIndicator(); return;
  }
  if (!m_indicatorMotion.retarget(target)) { paintIndicator(); return; }
  m_indicatorAnimation = animationManager()->animateProgress(0, 1, controls.segmented_transition_ms,
      [this](float progress) {
        auto settings = Style::controls();
        const auto& motion = MotionService::instance();
        if (motion.style() == MotionStyle::Linear) settings.segmented_travel_stretch = 0;
        const float native = Style::controlBezier(progress, settings.segmented_curve_x1, settings.segmented_curve_y1,
                                                  settings.segmented_curve_x2, settings.segmented_curve_y2);
        m_indicatorMotion.advance(progress, settings, width(), height(), motion.easedProgress(progress, native));
        paintIndicator();
      }, [this] { m_indicatorAnimation = 0; }, this);
  markPaintDirty();
}

void Segmented::refreshPresentation() {
  applyOuterStyle();
  refreshVariants();
  updateIndicator(false);
  markLayoutDirty();
}
