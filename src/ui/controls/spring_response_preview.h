#pragma once
#include "render/scene/node.h"
#include "ui/controls/spring_response_geometry.h"
#include "ui/signal.h"
#include <array>
class Box; class RectNode; class TextNode; class Renderer;
class SpringResponsePreview : public Node {
public:
  SpringResponsePreview();
  void setParameters(spring_response::Parameters parameters);
  void setScale(float scale);
  [[nodiscard]] spring_response::Parameters parameters() const noexcept { return m_parameters; }
protected:
  LayoutSize doMeasure(Renderer&,const LayoutConstraints&) override;
  void doArrange(Renderer&,const LayoutRect&) override;
  void doLayout(Renderer&) override;
private:
  void refresh(); void refreshColors();
  spring_response::Parameters m_parameters{}; float m_scale=1;
  Box* m_background=nullptr;
  std::array<RectNode*,64> m_curve{}; std::array<RectNode*,6> m_grid{};
  std::array<TextNode*,3> m_labels{};
  Signal<>::ScopedConnection m_materialConn,m_paletteConn;
};
