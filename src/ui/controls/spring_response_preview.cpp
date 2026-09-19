#include "ui/controls/spring_response_preview.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/box.h"
#include "ui/palette.h"
#include "ui/style.h"
#include <cmath>
#include <format>
#include <memory>
namespace {
  struct Point { float x,y; };
  void line(RectNode* node,Point a,Point b,float thickness) {
    const float dx=b.x-a.x,dy=b.y-a.y,length=std::hypot(dx,dy);
    node->setPosition((a.x+b.x-length)*.5F,(a.y+b.y-thickness)*.5F);
    node->setFrameSize(length,thickness); node->setRotation(std::atan2(dy,dx));
  }
  void ink(RectNode* node,ColorRole role,float alpha,float thickness) {
    RoundedRectStyle style; style.fill=colorForRole(role,alpha); style.radius=thickness*.5F;
    style.cornerPower=2; style.softness=1; node->setStyle(style);
  }
}
SpringResponsePreview::SpringResponsePreview() {
  const auto add=[this]<typename T>() { auto n=std::make_unique<T>(); n->setParticipatesInLayout(false);
    n->setHitTestVisible(false); return static_cast<T*>(addChild(std::move(n))); };
  m_background=add.operator()<Box>(); m_background->setMaterialIdentity("control","input");
  m_background->setSurfaceRelief(-.3F);
  for(auto& n:m_grid)n=add.operator()<RectNode>(); for(auto& n:m_curve)n=add.operator()<RectNode>();
  for(auto& n:m_labels)n=add.operator()<TextNode>();
  m_materialConn=Style::surfaceMaterialChanged().connect([this]{refresh();});
  m_paletteConn=paletteChanged().connect([this]{refreshColors();}); setSize(320,160); refresh();
}
void SpringResponsePreview::setParameters(spring_response::Parameters p) { m_parameters=p; refresh(); }
void SpringResponsePreview::setScale(float scale) { if(!std::isfinite(scale))return; m_scale=std::clamp(scale,.25F,4.F); markLayoutDirty(); refresh(); }
LayoutSize SpringResponsePreview::doMeasure(Renderer&,const LayoutConstraints& c){return c.constrain({320*m_scale,160*m_scale});}
void SpringResponsePreview::doArrange(Renderer&,const LayoutRect& r){setPosition(r.x,r.y);setFrameSize(r.width,r.height);refresh();}
void SpringResponsePreview::doLayout(Renderer&){refresh();}
void SpringResponsePreview::refresh(){
  m_background->setSize(width(),height()); const float px=24*m_scale,py=18*m_scale;
  const float w=std::max(0.F,width()-2*px),h=std::max(0.F,height()-2*py-14*m_scale);
  const auto sampled=spring_response::sample(m_parameters);
  const auto point=[&](std::size_t i,float value){return Point{px+w*static_cast<float>(i)/64,
    py+h*(sampled.maximum-value)/(sampled.maximum-sampled.minimum)};};
  for(std::size_t i=0;i<m_curve.size();++i)line(m_curve[i],point(i,sampled.values[i]),point(i+1,sampled.values[i+1]),2*m_scale);
  for(std::size_t i=0;i<5;++i){float x=px+w*static_cast<float>(i)/4;line(m_grid[i],{x,py},{x,py+h},m_scale);}
  const float target=py+h*(sampled.maximum-1)/(sampled.maximum-sampled.minimum);line(m_grid[5],{px,target},{px+w,target},m_scale);
  m_labels[0]->setText("0 s"); m_labels[1]->setText(std::format("{:.2g} s",sampled.horizon)); m_labels[2]->setText("1");
  m_labels[0]->setPosition(px,py+h+2*m_scale);m_labels[1]->setPosition(px+w-48*m_scale,py+h+2*m_scale);m_labels[2]->setPosition(4*m_scale,target-7*m_scale);
  for(auto* l:m_labels){l->setFrameSize(48*m_scale,14*m_scale);l->setFontSize(Style::fontSizeCaption*m_scale*.8F);} refreshColors();
}
void SpringResponsePreview::refreshColors(){m_background->setFill(colorSpecFromRole(ColorRole::Surface));m_background->setRadius(Style::scaledRadius(Style::radiusMd,m_scale));m_background->setBorder(colorSpecFromRole(ColorRole::Outline),m_scale);for(auto* n:m_grid)ink(n,ColorRole::Outline,.5F,m_scale);for(auto* n:m_curve)ink(n,ColorRole::Primary,1,2*m_scale);for(auto* l:m_labels)l->setColor(colorForRole(ColorRole::OnSurfaceVariant));}
