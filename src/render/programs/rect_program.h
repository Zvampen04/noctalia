#pragma once

#include "render/core/mat3.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>
#include <array>

struct RoundedRectStyle;

class RectProgram {
public:
  RectProgram() = default;
  ~RectProgram() = default;

  RectProgram(const RectProgram&) = delete;
  RectProgram& operator=(const RectProgram&) = delete;

  void ensureInitialized();
  void destroy();
  void abandon() noexcept;

  void draw(
      float surfaceWidth, float surfaceHeight, float width, float height, const RoundedRectStyle& style,
      const Mat3& transform = Mat3::identity()
  ) const;

private:
  ShaderProgram m_program;
  struct Backdrop { GLuint texture = 0; int width = 0, height = 0; };
  // Separate aspect buckets keep a wide bar and tall side panel from growing
  // one mostly empty, monitor-sized allocation. Reused on every draw.
  mutable std::array<Backdrop, 3> m_backdrops{};
  GLint m_glassLocation=-1, m_backdropLocation=-1, m_copyBoundsLocation=-1;
  GLint m_backdropSizeLocation=-1, m_displacementAxesLocation=-1;
  GLint m_positionLocation = -1;
  GLint m_surfaceSizeLocation = -1;
  GLint m_quadSizeLocation = -1;
  GLint m_rectOriginLocation = -1;
  GLint m_rectSizeLocation = -1;
  GLint m_paintClipLocation = -1, m_paintClipRadiusLocation = -1;
  GLint m_colorLocation = -1;
  GLint m_borderColorLocation = -1;
  GLint m_fillModeLocation = -1;
  GLint m_gradientDirectionLocation = -1;
  GLint m_gradientStopsLocation = -1;
  GLint m_gradientColor0Location = -1;
  GLint m_gradientColor1Location = -1;
  GLint m_gradientColor2Location = -1;
  GLint m_gradientColor3Location = -1;
  GLint m_cornerShapesLocation = -1;
  GLint m_logicalInsetLocation = -1;
  GLint m_cornerPowerLocation = -1;
  GLint m_paintClipPowerLocation = -1;
  GLint m_shadowExclusionPowerLocation = -1;
  GLint m_radiiLocation = -1;
  GLint m_softnessLocation = -1;
  GLint m_noAaLocation = -1;
  GLint m_invertFillLocation = -1;
  GLint m_frameChamferedLocation = -1, m_frameChamfersLocation = -1;
  GLint m_frameEnabledLocation = -1, m_frameShelfRectsLocation = -1, m_frameShelfShapesLocation = -1;
  GLint m_frameBorderColorsLocation = -1, m_frameBorderShapesLocation = -1;
  GLint m_segmentKindLocation = -1, m_segmentDepthLocation = -1, m_segmentVerticalLocation = -1;
  GLint m_borderWidthLocation = -1;
  GLint m_materialLocation = -1;
  GLint m_materialLightLocation = -1;
  GLint m_materialPlateauLocation = -1;
  GLint m_materialContactLocation = -1, m_materialPlateauShapeLocation = -1, m_materialPlateauFaceLocation = -1;
  GLint m_materialOpticalLocation = -1;
  GLint m_materialOpticalStyleLocation = -1;
  GLint m_materialOpticalLightLocation = -1, m_materialOpticalColorLocation = -1;
  GLint m_materialOpticalLensLocation = -1;
  GLint m_materialIllustrationLocation = -1;
  GLint m_materialPaintLocation = -1;
  GLint m_outerShadowLocation = -1;
  GLint m_shadowCutoutOffsetLocation = -1;
  GLint m_shadowExclusionLocation = -1;
  GLint m_shadowExclusionOffsetLocation = -1;
  GLint m_shadowExclusionSizeLocation = -1;
  GLint m_shadowExclusionCornerShapesLocation = -1;
  GLint m_shadowExclusionLogicalInsetLocation = -1;
  GLint m_shadowExclusionRadiiLocation = -1;
  GLint m_transformLocation = -1;
};
