#include "ui/controls/image_carousel.h"
#include "ui/controls/image.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/animation/animation_manager.h"
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
class SliceInput : public InputArea {
public:
  float skew = 0.0F;
protected:
  bool containsLocalPoint(float x, float y, bool) const override {
    return ui::carouselContains(x, y, width(), height(), skew);
  }
};
float mix(float a, float b, float t) { return a + (b-a)*t; }
}

ImageCarousel::ImageCarousel() {
  setClipChildren(true);
  m_paletteConnection = paletteChanged().connect([this] { applyPalette(); });
}
ImageCarousel::~ImageCarousel() {
  if (animationManager()) animationManager()->cancelForOwner(this);
}
void ImageCarousel::setPaths(const std::vector<std::string>& paths) {
  if (paths == m_paths) return;
  if (animationManager()) animationManager()->cancelForOwner(this);
  m_animation = 0;
  for (auto& slice : m_slices) removeChild(slice.input);
  m_slices.clear();
  m_paths.assign(paths.begin(), paths.begin()+std::min<std::size_t>(paths.size(),256));
  m_selected = std::clamp(m_selected,0,std::max(0,static_cast<int>(m_paths.size())-1));
  for (std::size_t i=0;i<m_paths.size();++i) {
    auto area = std::make_unique<SliceInput>();
    area->setFocusable(false);
    area->setEnabled(m_enabled);
    area->setParticipatesInLayout(false);
    area->setOnClick([this,i](const InputArea::PointerData&) {
      if (!m_enabled) return;
      if (static_cast<int>(i)==m_selected) { if(m_onActivate) m_onActivate(m_selected); }
      else selectFromInput(static_cast<int>(i));
    });
    area->setOnAxisHandler([this](const InputArea::PointerData& pointer) {
      if (!m_enabled) return true;
      const float steps=pointer.scrollSteps();
      if (steps!=0.0F) selectFromInput(m_selected+(steps>0.0F?1:-1));
      return true;
    });
    auto image = std::make_unique<Image>();
    image->setFit(ImageFit::Cover);
    image->setHitTestVisible(false);
    auto* picture=static_cast<Image*>(area->addChild(std::move(image)));
    auto* input=static_cast<InputArea*>(addChild(std::move(area)));
    m_slices.push_back(Slice{.input=input,.image=picture});
  }
  m_laidOut=false;
  applyPalette();
  markLayoutDirty();
}
void ImageCarousel::setSelected(int index) {
  index=std::clamp(index,0,std::max(0,static_cast<int>(m_paths.size())-1));
  if (index==m_selected) return;
  m_selected=index;
  retarget(m_laidOut);
  applyPalette();
  markLayoutDirty();
}
void ImageCarousel::selectFromInput(int index) {
  index=std::clamp(index,0,std::max(0,static_cast<int>(m_paths.size())-1));
  if(index==m_selected) return;
  setSelected(index);
  if(m_onSelect) m_onSelect(index);
}
void ImageCarousel::setEnabled(bool enabled) {
  m_enabled=enabled;
  for(auto& slice:m_slices) slice.input->setEnabled(enabled);
}
void ImageCarousel::setGeometry(float ew,float eh,float sw,float sh,float spacing,float skew,float duration) {
  auto finite=[](float value,float fallback){return std::isfinite(value)?value:fallback;};
  ew=finite(ew,768);eh=finite(eh,475);sw=finite(sw,108);sh=finite(sh,432);spacing=finite(spacing,-30);skew=finite(skew,28);duration=finite(duration,220);
  ew=std::clamp(ew,1.0F,4096.0F); eh=std::clamp(eh,1.0F,2160.0F);
  sw=std::clamp(sw,1.0F,1024.0F); sh=std::clamp(sh,1.0F,eh);
  spacing=std::clamp(spacing,1.0F-sw,1024.0F); skew=std::clamp(skew,-sw+1.0F,sw-1.0F);
  duration=std::clamp(duration,0.0F,2000.0F);
  if(m_expandedWidth==ew&&m_expandedHeight==eh&&m_sliceWidth==sw&&m_sliceHeight==sh&&m_spacing==spacing&&m_skew==skew&&m_duration==duration) return;
  m_expandedWidth=ew;m_expandedHeight=eh;m_sliceWidth=sw;m_sliceHeight=sh;m_spacing=spacing;m_skew=skew;m_duration=duration;
  retarget(false);markLayoutDirty();
}
void ImageCarousel::retarget(bool animate) {
  if(animationManager()) animationManager()->cancelForOwner(this);
  m_animation=0;
  for(std::size_t i=0;i<m_slices.size();++i) {
    auto& slice=m_slices[i];slice.from=slice.current;
    slice.target=ui::carouselSlice(static_cast<int>(i),m_selected,width(),m_expandedWidth,m_expandedHeight,m_sliceWidth,m_sliceHeight,m_spacing);
  }
  if(animate&&animationManager()&&m_duration>0.0F) {
    m_animation=animationManager()->animate(0.0F,1.0F,m_duration,Easing::EaseOutCubic,
        [this](float progress){place(progress);},[this]{m_animation=0;},this);
  } else place(1.0F);
}
void ImageCarousel::place(float t) {
  for(auto& slice:m_slices) {
    const auto& a=slice.from;const auto& b=slice.target;
    slice.current={mix(a.x,b.x,t),mix(a.y,b.y,t),mix(a.width,b.width,t),mix(a.height,b.height,t),b.z};
    const auto& c=slice.current;
    slice.input->setPosition(c.x,c.y);slice.input->setSize(c.width,c.height);slice.input->setZIndex(c.z);
    static_cast<SliceInput*>(slice.input)->skew=m_skew;
    slice.image->setSize(c.width,c.height);slice.image->setSliceSkew(m_skew);
  }
  markPaintDirty();
}
void ImageCarousel::applyPalette() {
  for(std::size_t i=0;i<m_slices.size();++i) {
    const bool selected=static_cast<int>(i)==m_selected;
    m_slices[i].image->setBorder(colorSpecFromRole(selected?ColorRole::Primary:ColorRole::Outline,selected?1.0F:0.5F),selected?3.0F:1.0F);
    const float dim=selected?1.0F:0.58F;
    m_slices[i].image->setTint(Color{dim,dim,dim,1.0F});
  }
  markPaintDirty();
}
void ImageCarousel::doLayout(Renderer& renderer) {
  if(!m_laidOut||m_layoutWidth!=width()) {
    m_layoutWidth=width();retarget(false);m_laidOut=true;
  }
  for(std::size_t i=0;i<m_slices.size();++i) {
    auto& slice=m_slices[i];
    const bool nearby=std::abs(static_cast<int>(i)-m_selected)<=8;
    slice.input->setVisible(nearby);
    const int target=static_cast<int>(std::ceil(std::min(m_expandedWidth,1536.0F)));
    if(nearby&&(!slice.loaded||slice.requestedSize!=target||slice.requestedScale!=renderer.renderScale())&&!m_paths[i].empty()) {
      // Image applies output scaling itself. Reuse the shell's one asynchronous cache.
      slice.loaded=m_textureCache?slice.image->setSourceFileAsync(renderer,*m_textureCache,m_paths[i],target,true)
                                 :slice.image->setSourceFile(renderer,m_paths[i],target,true);
      slice.requestedSize=target;slice.requestedScale=renderer.renderScale();
    }
    if(nearby) slice.image->layout(renderer);
  }
}
LayoutSize ImageCarousel::doMeasure(Renderer&,const LayoutConstraints& constraints) {
  return {std::clamp(width()>0.0F?width():m_expandedWidth+6*(m_sliceWidth+m_spacing),constraints.minWidth,constraints.hasMaxWidth?constraints.maxWidth:100000.0F),
          std::clamp(m_expandedHeight,constraints.minHeight,constraints.hasMaxHeight?constraints.maxHeight:100000.0F)};
}
void ImageCarousel::doArrange(Renderer& renderer,const LayoutRect& rect) {
  setPosition(rect.x,rect.y);setSize(rect.width,rect.height);doLayout(renderer);
}
