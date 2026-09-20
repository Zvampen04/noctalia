#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"
#include "ui/controls/image_carousel_layout.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Image;
class AsyncTextureCache;
class InputArea;

// A retained image selector. Geometry and image masking share logical units;
// scripts supply data and selection, never per-frame rendering commands.
class ImageCarousel : public Node {
public:
  ImageCarousel();
  ~ImageCarousel() override;
  void setPaths(const std::vector<std::string>& paths);
  void setTextureCache(AsyncTextureCache* cache) { m_textureCache=cache; }
  void setSelected(int index);
  void setEnabled(bool enabled);
  void setGeometry(float expandedWidth, float expandedHeight, float sliceWidth,
                   float sliceHeight, float spacing, float skew, float durationMs);
  void setOnSelect(std::function<void(int)> callback) { m_onSelect = std::move(callback); }
  void setOnActivate(std::function<void(int)> callback) { m_onActivate = std::move(callback); }

private:
  struct Slice {
    InputArea* input = nullptr;
    Image* image = nullptr;
    ui::CarouselSlice from{}, current{}, target{};
    bool loaded = false;
    int requestedSize = 0;
    float requestedScale = 0.0F;
  };
  void doLayout(Renderer& renderer) override;
  LayoutSize doMeasure(Renderer&, const LayoutConstraints& constraints) override;
  void doArrange(Renderer& renderer, const LayoutRect& rect) override;
  void retarget(bool animate);
  void place(float progress);
  void applyPalette();
  void selectFromInput(int index);
  AsyncTextureCache* m_textureCache = nullptr;
  std::vector<std::string> m_paths;
  std::vector<Slice> m_slices;
  int m_selected = 0;
  bool m_enabled = true;
  bool m_laidOut = false;
  float m_layoutWidth = 0.0F;
  float m_expandedWidth = 768.0F, m_expandedHeight = 475.0F;
  float m_sliceWidth = 108.0F, m_sliceHeight = 432.0F;
  float m_spacing = -30.0F, m_skew = 28.0F, m_duration = 220.0F;
  std::uint32_t m_animation = 0;
  std::function<void(int)> m_onSelect, m_onActivate;
  Signal<>::ScopedConnection m_paletteConnection;
};
