#pragma once

#include "render/core/color.h"
#include "render/core/render_styles.h"
#include "render/scene/node.h"

#include <algorithm>

class CountdownRingNode : public Node {
public:
  CountdownRingNode() : Node(NodeType::CountdownRing) {}

  void setColor(const Color& color) {
    if (m_style.color == color) {
      return;
    }
    m_style.color = color;
    markPaintDirty();
  }
  void setThickness(float thickness) {
    if (m_style.thickness == thickness) {
      return;
    }
    m_style.thickness = thickness;
    markPaintDirty();
  }
  void setProgress(float progress) {
    const float clamped = std::clamp(progress, 0.0F, 1.0F);
    if (m_style.progress == clamped) {
      return;
    }
    m_style.progress = clamped;
    markPaintDirty();
  }

  void setRadius(float radius) {
    if (m_style.radius == radius) return;
    m_style.radius = radius;
    markPaintDirty();
  }
  void setSymmetric(bool symmetric) {
    if (m_style.symmetric == symmetric) return;
    m_style.symmetric = symmetric;
    markPaintDirty();
  }
  void setStyle(const CountdownRingStyle& style) {
    if (m_style == style) return;
    m_style = style;
    markPaintDirty();
  }

  [[nodiscard]] const CountdownRingStyle& style() const noexcept { return m_style; }

private:
  CountdownRingStyle m_style;
};
