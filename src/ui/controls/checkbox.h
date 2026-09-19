#pragma once

#include "render/scene/node.h"
#include "ui/palette.h"

#include <functional>
#include <optional>

class Box;
class Glyph;
class InputArea;

class Checkbox : public Node {
public:
  Checkbox();

  void setChecked(bool checked);
  void setEnabled(bool enabled);
  void setOnChange(std::function<void(bool)> callback);
  void setScale(float scale);
  void setCheckedColors(std::optional<ColorSpec> fill, std::optional<ColorSpec> border, std::optional<ColorSpec> glyph);

  [[nodiscard]] bool checked() const noexcept { return m_checked; }
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] bool hovered() const noexcept;
  [[nodiscard]] bool pressed() const noexcept;

private:
  void doLayout(Renderer& renderer) override;
  void applyState();

  Box* m_box = nullptr;
  Box* m_plateau = nullptr;
  float m_checkedProgress = 0.0F;
  std::uint32_t m_animId = 0;
  Signal<>::ScopedConnection m_materialConn;
  Signal<>::ScopedConnection m_paletteConn;
  Glyph* m_checkGlyph = nullptr;
  InputArea* m_inputArea = nullptr;
  std::function<void(bool)> m_onChange;
  std::optional<ColorSpec> m_checkedFill;
  std::optional<ColorSpec> m_checkedBorder;
  std::optional<ColorSpec> m_checkedGlyph;
  bool m_checked = false;
  bool m_enabled = true;
  float m_scale = 1.0F;
};
