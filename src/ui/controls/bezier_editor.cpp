#include "ui/controls/bezier_editor.h"

#include "core/input/key_modifiers.h"
#include "cursor-shape-v1-client-protocol.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {
void line(RectNode* node, bezier_editor::Point from, bezier_editor::Point to, float thickness) {
  const float dx = to.x - from.x, dy = to.y - from.y;
  const float length = std::hypot(dx, dy);
  node->setPosition((from.x + to.x - length) * 0.5F, (from.y + to.y - thickness) * 0.5F);
  node->setFrameSize(length, thickness);
  node->setRotation(std::atan2(dy, dx));
}
void ink(RectNode* node, ColorRole role, float alpha, float thickness) {
  RoundedRectStyle style;
  style.fill = colorForRole(role, alpha);
  style.radius = thickness * 0.5F;
  style.cornerPower = 2.0F;
  style.softness = 1;
  node->setStyle(style);
}
bool arrow(std::uint32_t sym) {
  return sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_Up || sym == XKB_KEY_Down;
}
} // namespace

BezierEditor::BezierEditor() {
  const auto add = [this]<typename T>() {
    auto child = std::make_unique<T>();
    child->setParticipatesInLayout(false);
    child->setHitTestVisible(false);
    return static_cast<T*>(addChild(std::move(child)));
  };
  m_background = add.operator()<Box>();
  m_background->setMaterialIdentity("control", "input");
  m_background->setSurfaceRelief(-0.3F);
  for (auto& grid : m_gridLines) grid = add.operator()<RectNode>();
  for (auto& control : m_controlLines) control = add.operator()<RectNode>();
  for (auto& segment : m_curveLines) segment = add.operator()<RectNode>();
  for (auto& endpoint : m_endpoints) endpoint = add.operator()<Box>();
  for (auto& handle : m_handles) {
    handle = add.operator()<Box>();
    handle->setMaterialIdentity("control", "slider");
  }
  for (std::size_t i = 0; i < m_handleLabels.size(); ++i) {
    m_handleLabels[i] = add.operator()<TextNode>();
    m_handleLabels[i]->setText(std::to_string(i+1));
    m_handleLabels[i]->setTextAlign(TextAlign::Center);
  }
  const std::array<const char*, 4> labels{"0", "1", "2", "−2"};
  for (std::size_t i = 0; i < labels.size(); ++i) {
    m_axisLabels[i] = add.operator()<TextNode>();
    m_axisLabels[i]->setText(labels[i]);
  }
  m_input = add.operator()<InputArea>();
  m_input->setHitTestVisible(true);
  m_input->setFocusable(true);
  m_input->setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR);
  m_input->setOnFocusGain([this] { refreshColors(); });
  m_input->setOnFocusLoss([this] { endEdit(m_dragging); refreshColors(); });
  m_input->setOnCancel([this] { endEdit(true); });
  m_input->setOnKeyDown([this](const InputArea::KeyData& data) { key(data.sym, data.modifiers, true); });
  m_input->setOnKeyUp([this](const InputArea::KeyData& data) { key(data.sym, data.modifiers, false); });
  m_input->setOnPress([this](const InputArea::PointerData& data) {
    if (!m_enabled || data.button != BTN_LEFT) return;
    if (!data.pressed) { endEdit(false); return; }
    endEdit(false);
    const auto view = viewport();
    std::array<bezier_editor::Point, 2> handles{
        bezier_editor::toScreen({m_curve[0], m_curve[1]}, view),
        bezier_editor::toScreen({m_curve[2], m_curve[3]}, view)};
    const std::array<float, 2> distances{
        std::hypot(data.localX - handles[0].x, data.localY - handles[0].y),
        std::hypot(data.localX - handles[1].x, data.localY - handles[1].y)};
    const float hitRadius = 18 * m_scale;
    // Preserve keyboard selection for coincident handles and subpixel ties.
    // A quarter logical pixel avoids fractional-scale rounding selecting the other handle.
    if (std::abs(distances[0] - distances[1]) > 0.25F * m_scale
        || (distances[m_selected] > hitRadius && std::min(distances[0], distances[1]) <= hitRadius))
      m_selected = distances[1] < distances[0] ? 1 : 0;
    if (distances[m_selected] <= hitRadius) {
      m_dragBaseline = m_curve;
      m_dragOffset = {data.localX - handles[m_selected].x, data.localY - handles[m_selected].y};
      m_dragging = true;
    }
    refreshColors();
  });
  m_input->setOnMotion([this](const InputArea::PointerData& data) {
    if (m_enabled && m_dragging)
      moveSelected(bezier_editor::fromScreen({data.localX - m_dragOffset.x, data.localY - m_dragOffset.y}, viewport()));
  });
  m_materialConn = Style::surfaceMaterialChanged().connect([this] { refresh(); });
  m_paletteConn = paletteChanged().connect([this] { refreshColors(); });
  setSize(320, 220);
  refresh();
}

void BezierEditor::setCurve(Curve curve) {
  if (!bezier_editor::normalize(curve) || curve == m_curve) return;
  m_curve = curve;
  if (m_dragging || m_keyboardEditing) m_dragBaseline = curve;
  refresh();
}
void BezierEditor::setOnChanged(std::function<void(Curve)> callback) { m_onChanged = std::move(callback); }
void BezierEditor::setOnEditEnd(std::function<void()> callback) { m_onEditEnd = std::move(callback); }
void BezierEditor::setEnabled(bool enabled) {
  if (enabled == m_enabled) return;
  endEdit(true);
  m_enabled = enabled;
  m_input->setEnabled(enabled);
  setOpacity(enabled ? 1 : 0.45F);
  refreshColors();
}
void BezierEditor::setScale(float scale) {
  if (!std::isfinite(scale)) return;
  scale = std::clamp(scale, 0.25F, 4.0F);
  if (m_scale == scale) return;
  m_scale = scale;
  markLayoutDirty();
  refresh();
}
LayoutSize BezierEditor::doMeasure(Renderer&, const LayoutConstraints& constraints) {
  return constraints.constrain({320 * m_scale, 220 * m_scale});
}
void BezierEditor::doArrange(Renderer&, const LayoutRect& rect) {
  setPosition(rect.x, rect.y);
  setFrameSize(rect.width, rect.height);
  refresh();
}
void BezierEditor::doLayout(Renderer&) { refresh(); }
bezier_editor::Viewport BezierEditor::viewport() const {
  const float padding = 24 * m_scale;
  return {padding, padding, std::max(0.0F, width() - 2*padding), std::max(0.0F, height() - 2*padding)};
}
void BezierEditor::moveSelected(bezier_editor::Point point) {
  if (!bezier_editor::moveHandle(m_curve, m_selected, point)) return;
  refresh();
  if (m_onChanged) m_onChanged(m_curve);
}
void BezierEditor::endEdit(bool cancel) {
  if (!m_dragging && !m_keyboardEditing) return;
  m_dragging = m_keyboardEditing = false;
  if (cancel && m_curve != m_dragBaseline) {
    m_curve = m_dragBaseline;
    refresh();
    if (m_onChanged) m_onChanged(m_curve);
  }
  refreshColors();
  if (m_onEditEnd) m_onEditEnd();
}
void BezierEditor::key(std::uint32_t sym, std::uint32_t modifiers, bool pressed) {
  if (!m_enabled) return;
  if (!pressed) { if (arrow(sym) && m_keyboardEditing) endEdit(false); return; }
  if (sym == XKB_KEY_Escape) { endEdit(true); return; }
  if ((modifiers & ~KeyMod::Shift) != 0) return;
  if (sym == XKB_KEY_1 || sym == XKB_KEY_Home || sym == XKB_KEY_2 || sym == XKB_KEY_End
      || sym == XKB_KEY_space || sym == XKB_KEY_Return) {
    endEdit(false);
    if (sym == XKB_KEY_1 || sym == XKB_KEY_Home) m_selected = 0;
    else if (sym == XKB_KEY_2 || sym == XKB_KEY_End) m_selected = 1;
    else m_selected = 1-m_selected;
    refreshColors();
    return;
  }
  if (!arrow(sym)) return;
  if (!m_keyboardEditing) { m_dragBaseline = m_curve; m_keyboardEditing = true; }
  const float step = (modifiers & KeyMod::Shift) != 0 ? 0.1F : 0.01F;
  bezier_editor::Point point{m_curve[m_selected*2], m_curve[m_selected*2+1]};
  if (sym == XKB_KEY_Left) point.x -= step;
  if (sym == XKB_KEY_Right) point.x += step;
  if (sym == XKB_KEY_Down) point.y -= step;
  if (sym == XKB_KEY_Up) point.y += step;
  moveSelected(point);
}
void BezierEditor::refresh() {
  const auto view = viewport();
  m_background->setSize(width(), height());
  m_input->setFrameSize(width(), height());
  const auto screen = [&](float x, float y) { return bezier_editor::toScreen({x,y}, view); };
  line(m_gridLines[0], screen(0,-2), screen(0,2), m_scale);
  line(m_gridLines[1], screen(1,-2), screen(1,2), m_scale);
  line(m_gridLines[2], screen(0,0), screen(1,0), m_scale);
  line(m_gridLines[3], screen(0,1), screen(1,1), m_scale);
  const auto points = bezier_editor::polyline(m_curve, view);
  for (std::size_t i = 0; i < m_curveLines.size(); ++i) line(m_curveLines[i], points[i], points[i+1], 2*m_scale);
  const float handleSize = 18*m_scale, endpointSize = 6*m_scale;
  for (std::size_t i = 0; i < 2; ++i) {
    const auto handle = screen(m_curve[i*2], m_curve[i*2+1]);
    const auto endpoint = screen(static_cast<float>(i), static_cast<float>(i));
    line(m_controlLines[i], endpoint, handle, m_scale);
    m_handles[i]->setPosition(handle.x-handleSize/2, handle.y-handleSize/2);
    m_handles[i]->setSize(handleSize, handleSize);
    m_endpoints[i]->setPosition(endpoint.x-endpointSize/2, endpoint.y-endpointSize/2);
    m_endpoints[i]->setSize(endpointSize, endpointSize);
    m_handleLabels[i]->setPosition(handle.x-handleSize/2, handle.y-handleSize/2);
    m_handleLabels[i]->setFrameSize(handleSize, handleSize);
    m_handleLabels[i]->setFontSize(Style::fontSizeCaption*m_scale*0.9F);
  }
  const std::array<bezier_editor::Point,4> labels{screen(0,0),screen(1,1),screen(0,2),screen(0,-2)};
  for (std::size_t i = 0; i < labels.size(); ++i) {
    m_axisLabels[i]->setPosition(labels[i].x-18*m_scale, labels[i].y-6*m_scale);
    m_axisLabels[i]->setFrameSize(16*m_scale, 14*m_scale);
    m_axisLabels[i]->setFontSize(Style::fontSizeCaption*m_scale*0.8F);
  }
  refreshColors();
}
void BezierEditor::refreshColors() {
  m_background->setFill(colorSpecFromRole(ColorRole::Surface));
  m_background->setRadius(Style::scaledRadius(Style::radiusMd,m_scale));
  m_background->setBorder(colorSpecFromRole(m_input->focused() ? ColorRole::Primary : ColorRole::Outline), m_scale);
  for (auto* grid : m_gridLines) ink(grid, ColorRole::Outline, 0.5F, m_scale);
  for (auto* control : m_controlLines) ink(control, ColorRole::Secondary, 0.8F, m_scale);
  for (auto* segment : m_curveLines) ink(segment, ColorRole::Primary, 1, 2*m_scale);
  for (std::size_t i = 0; i < 2; ++i) {
    m_handles[i]->setFill(colorSpecFromRole(ColorRole::Primary));
    m_handles[i]->setRadius(Style::scaledRadius(9,m_scale));
    m_handles[i]->setCornerPower(2.0F);
    m_handles[i]->setBorder(colorSpecFromRole(m_selected == i ? ColorRole::OnSurface : ColorRole::Outline),
                            (m_selected == i && m_input->focused() ? 2.0F : 1.0F)*m_scale);
    m_handles[i]->setSurfaceRelief(m_dragging && m_selected == i ? -0.6F : 0.6F);
    m_endpoints[i]->setFill(colorSpecFromRole(ColorRole::OnSurface));
    m_endpoints[i]->setRadius(Style::scaledRadius(3,m_scale));
    m_endpoints[i]->setCornerPower(2.0F);
    m_handleLabels[i]->setColor(colorForRole(ColorRole::OnPrimary));
  }
  for (auto* label : m_axisLabels) label->setColor(colorForRole(ColorRole::OnSurface));
}
