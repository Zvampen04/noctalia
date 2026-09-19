#pragma once

#include "render/scene/node.h"
#include "ui/signal.h"
#include "ui/controls/bezier_editor_geometry.h"
#include <array>
#include <functional>

class Box;
class RectNode;
class TextNode;
class InputArea;

class BezierEditor : public Node {
public:
  using Curve = bezier_editor::Curve;
  BezierEditor();
  void setCurve(Curve curve);
  [[nodiscard]] const Curve& curve() const noexcept { return m_curve; }
  void setOnChanged(std::function<void(Curve)> callback);
  void setOnEditEnd(std::function<void()> callback);
  void setEnabled(bool enabled);
  void setScale(float scale);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] bool dragging() const noexcept { return m_dragging; }
  [[nodiscard]] std::size_t selectedHandle() const noexcept { return m_selected; }
  [[nodiscard]] InputArea* inputArea() const noexcept { return m_input; }

protected:
  LayoutSize doMeasure(Renderer&, const LayoutConstraints& constraints) override;
  void doArrange(Renderer&, const LayoutRect& rect) override;
  void doLayout(Renderer&) override;

private:
  [[nodiscard]] bezier_editor::Viewport viewport() const;
  void refresh();
  void refreshColors();
  void moveSelected(bezier_editor::Point point);
  void endEdit(bool cancel);
  void key(std::uint32_t sym, std::uint32_t modifiers, bool pressed);
  Curve m_curve{0.34F, 0.8F, 0.34F, 1.0F};
  Curve m_dragBaseline{};
  bezier_editor::Point m_dragOffset{};
  std::size_t m_selected = 0;
  bool m_enabled = true, m_dragging = false, m_keyboardEditing = false;
  float m_scale = 1;
  Box* m_background = nullptr;
  std::array<RectNode*, 64> m_curveLines{};
  std::array<RectNode*, 4> m_gridLines{};
  std::array<RectNode*, 2> m_controlLines{};
  std::array<Box*, 2> m_endpoints{}, m_handles{};
  std::array<TextNode*, 2> m_handleLabels{};
  std::array<TextNode*, 4> m_axisLabels{};
  InputArea* m_input = nullptr;
  std::function<void(Curve)> m_onChanged;
  std::function<void()> m_onEditEnd;
  Signal<>::ScopedConnection m_materialConn, m_paletteConn;
};
