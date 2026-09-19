#pragma once

#include "render/core/render_styles.h"
#include "render/scene/node.h"
#include <atomic>
#include <functional>
#include <string_view>

class RectNode : public Node {
public:
  RectNode() : Node(NodeType::Rect) {}

  [[nodiscard]] const RoundedRectStyle& style() const noexcept { return m_style; }
  [[nodiscard]] std::uint32_t materialSeed() const noexcept { return m_materialSeed; }

  void setMaterialRole(std::string_view role) { m_materialRole = role; }
  [[nodiscard]] std::string_view materialRole() const noexcept { return m_materialRole; }

  using MaterialResolver = std::function<noctalia::material::Parameters(std::string_view)>;
  void setMaterialResolver(MaterialResolver resolver) { m_materialResolver = std::move(resolver); }

  void setStyle(const RoundedRectStyle& style) {
    if (m_style == style) {
      return;
    }
    m_style = style;
    markPaintDirty();
  }

protected:
  void doMaterialSurfaceChanged() override {
    if (!m_materialResolver || !m_style.material) return;
    auto updated = m_style;
    updated.material = m_materialResolver(materialSurfaceName());
    setStyle(updated);
  }

private:
  std::string m_materialRole = "surface";
  MaterialResolver m_materialResolver;
  inline static std::atomic<std::uint32_t> s_nextMaterialSeed{1};
  const std::uint32_t m_materialSeed = s_nextMaterialSeed.fetch_add(1, std::memory_order_relaxed);
  RoundedRectStyle m_style;
};
