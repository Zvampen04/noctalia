#include "ui/controls/button.h"
#include "ui/controls/box.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/checkbox.h"
#include "ui/controls/toggle.h"
#include "ui/controls/input.h"
#include "ui/style.h"
#include "render/animation/animation_manager.h"
#include "render/scene/rect_node.h"
#include <cmath>
#include <cassert>
int main() {
  const auto previous = Style::controls();
  Checkbox checkbox;
  Toggle toggle;
  Input input;
  checkbox.setChecked(true);
  checkbox.setEnabled(false);
  toggle.setCheckedImmediate(true);
  toggle.setEnabled(false);
  input.setValue("unchanged åä");
  input.moveCaretLeft(true);
  const auto caret = input.textInputState();
  auto settings = previous;
  settings.button_variant = Style::ButtonTreatment::RaisedInset;
  settings.toggle_variant = Style::ToggleTreatment::Sweep;
  settings.checkbox_variant = Style::CheckboxTreatment::Plateau;
  settings.card_variant = Style::CardTreatment::Raised;
  settings.button_transition_ms = settings.toggle_transition_ms = settings.checkbox_transition_ms = 0;
  Style::setControls(settings);
  assert(checkbox.checked() && !checkbox.enabled());
  assert(toggle.checked() && !toggle.enabled());
  assert(toggle.width() == 60 && toggle.height() == 30);
  const auto after = input.textInputState();
  assert(after.surroundingText == caret.surroundingText && after.cursor == caret.cursor && after.anchor == caret.anchor);
  toggle.setCheckedImmediate(false);
  checkbox.setChecked(false);
  Style::setControls(previous);
  assert(!checkbox.checked() && !checkbox.enabled() && !toggle.checked() && !toggle.enabled());
  Style::setControls(settings);
  assert(!checkbox.checked() && !toggle.checked());
  // No clock or renderer is needed: an active animation starts at the old endpoint.
  settings.toggle_transition_ms = 400;
  Style::setControls(settings);
  {
    AnimationManager animations;
    Toggle animated;
    animated.setAnimationManager(&animations);
    animated.setChecked(true);
    assert(animations.hasActive());
    animated.setScale(1.0F);
    bool oldEndpointVisible = false;
    for (const auto& child : animated.children())
      if (dynamic_cast<RectNode*>(child.get()) && child->width() == 120)
        oldEndpointVisible = child->x() == -90;
    assert(oldEndpointVisible); // Refresh must preserve progress, rather than snap to +30.
    animated.setCheckedImmediate(true); // Same logical value must still cancel the animation.
    assert(!animations.hasActive() && animated.checked());
    bool finalEndpointVisible = false;
    for (const auto& child : animated.children())
      if (dynamic_cast<RectNode*>(child.get()) && child->width() == 120)
        finalEndpointVisible = child->x() == 30;
    assert(finalEndpointVisible);
  }
  {
    Button button;
    button.setEnabled(false);
    bool fadedFace = false;
    for (const auto& child : button.children())
      if (const auto* face = dynamic_cast<RectNode*>(child.get()))
        fadedFace = std::abs(face->style().fill.a - 0.55F) < 0.0001F;
    assert(fadedFace);
    button.setEnabled(true);
    auto custom = Button::defaultPalette(ButtonVariant::Default);
    custom.normal.bg = fixedColorSpec(Color{0.17F, 0.23F, 0.31F, 1.0F});
    button.setCustomPalette(custom);
    bool retainedCustomFace = false;
    for (const auto& child : button.children())
      if (const auto* face = dynamic_cast<RectNode*>(child.get()))
        retainedCustomFace = std::abs(face->style().fill.r - 0.17F) < 0.0001F;
    assert(retainedCustomFace);
  }
  {
    settings.card_variant = Style::CardTreatment::Standard;
    Style::setControls(settings);
    const bool oldBorders = Style::cardBordersEnabled();
    Style::setCardBordersEnabled(false);
    Box inherited;
    Box explicitFalse;
    inherited.setCardStyle();
    explicitFalse.setCardStyle(1.0F, 1.0F, false);
    assert(inherited.style().borderWidth == 0 && explicitFalse.style().borderWidth == 0);
    Style::setCardBordersEnabled(true);
    assert(inherited.style().borderWidth == Style::borderWidth);
    assert(explicitFalse.style().borderWidth == 0);
    for (bool scroll : {false, true}) {
      std::unique_ptr<Flex> inheritedCard = scroll ? std::unique_ptr<Flex>(new ScrollView) : std::make_unique<Flex>();
      std::unique_ptr<Flex> explicitCard = scroll ? std::unique_ptr<Flex>(new ScrollView) : std::make_unique<Flex>();
      const auto configure = [scroll](Flex& card, std::optional<bool> border) {
        if (scroll) static_cast<ScrollView&>(card).setCardStyle(1, 1, border);
        else card.setCardStyle(1, 1, border);
      };
      const auto borderWidth = [](const Flex& card) {
        for (const auto& child : card.children())
          if (const auto* rectangle = dynamic_cast<const RectNode*>(child.get())) return rectangle->style().borderWidth;
        return -1.0F;
      };
      Style::setCardBordersEnabled(false);
      configure(*inheritedCard, std::nullopt);
      configure(*explicitCard, false);
      Style::setCardBordersEnabled(true);
      assert(borderWidth(*inheritedCard) == Style::borderWidth);
      assert(borderWidth(*explicitCard) == 0);
      settings.card_variant = Style::CardTreatment::Raised;
      Style::setControls(settings);
      assert(borderWidth(*inheritedCard) == 0);
      settings.card_variant = Style::CardTreatment::Standard;
      Style::setControls(settings);
      assert(borderWidth(*inheritedCard) == Style::borderWidth);
    }
    Style::setCardBordersEnabled(oldBorders);
  }
  {
    const auto oldOverrides = Style::materialOverrides();
    Style::MaterialOverrides overrides;
    overrides.families["card"].elevation = 3;
    overrides.families["container"].elevation = 2;
    overrides.families["panel"].elevation = 4;
    overrides.surfaces["bar.instance.default"].rim = 0.8F;
    Style::setMaterialOverrides(overrides);
    settings.card_variant = Style::CardTreatment::Raised;
    settings.card_relief = 2.5F;
    Style::setControls(settings);
    Box box;
    Flex flex;
    ScrollView scroll;
    Box explicitPanel;
    explicitPanel.setMaterialIdentity("surface", "panel", "system-dialogs");
    box.setCardStyle(); flex.setCardStyle(); scroll.setCardStyle(); explicitPanel.setCardStyle();
    const auto elevation = [](const Node& node) {
      for (const auto& child : node.children())
        if (const auto* rectangle = dynamic_cast<const RectNode*>(child.get()); rectangle && rectangle->style().material)
          return rectangle->style().material->plateau.elevation;
      return -999.0F;
    };
    assert(elevation(box) == 7.5F && elevation(flex) == 7.5F && elevation(scroll) == 7.5F);
    assert(elevation(explicitPanel) == 10 && explicitPanel.materialSurfaceName() == "system-dialogs");
    settings.card_variant = Style::CardTreatment::Standard;
    Style::setControls(settings);
    assert(elevation(box) == 1.5F && elevation(flex) == 1.5F && elevation(scroll) == 1.5F);
    assert(elevation(explicitPanel) == 3 && explicitPanel.materialSurfaceName() == "system-dialogs");
    Box pathBox;
    pathBox.setFill(Color{1, 1, 1, 1});
    pathBox.setMaterialIdentityPath(
        "surface", "bar-widget", {"bar", "bar.instance.default", "bar.widget.meter-a"}
    );
    const auto rim = [](const Node& node) {
      for (const auto& child : node.children())
        if (const auto* rectangle = dynamic_cast<const RectNode*>(child.get()); rectangle && rectangle->style().material)
          return rectangle->style().material->optical.rim;
      return -999.0F;
    };
    assert(rim(pathBox) == 0.8F);
    pathBox.setMaterialIdentity("surface", "bar-widget", "bar.widget.meter-a");
    assert(rim(pathBox) != 0.8F);
    Flex pathFlex;
    pathFlex.setFill(Color{1, 1, 1, 1});
    pathFlex.setMaterialIdentityPath(
        "surface", "bar-widget", {"bar", "bar.instance.default", "bar.widget.meter-a"}
    );
    assert(rim(pathFlex) == 0.8F);
    pathFlex.setMaterialIdentity("surface", "bar-widget", "bar.widget.meter-a");
    assert(rim(pathFlex) != 0.8F);
    Style::setMaterialOverrides(oldOverrides);
  }
  Style::setControls(previous);
}
