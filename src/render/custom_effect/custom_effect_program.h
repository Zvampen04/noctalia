#pragma once

#include "render/core/mat3.h"
#include "render/core/render_styles.h"
#include "render/core/shader_program.h"

#include <GLES2/gl2.h>
#include <optional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

class CustomEffectProgramCache {
public:
  [[nodiscard]] bool draw(
      float surfaceWidth, float surfaceHeight, float width, float height,
      const RoundedRectStyle& style, const Mat3& transform);

  [[nodiscard]] std::optional<CustomEffectCompileStatus> status(std::string_view stableId) const;
  void collectUnused();
  // Safe outside a render callback. Actual GL deletion is deferred until the
  // next collectUnused() with the backend context current.
  void release(std::string_view stableId);
  void destroy();
  void abandon() noexcept;

private:
  struct Program {
    ShaderProgram shader;
    std::string activeDigest;
    float maxSampleRadiusPx = 0.0F;
    GLint position = -1;
    GLint surfaceSize = -1;
    GLint quadSize = -1;
    GLint rectSize = -1;
    GLint transform = -1;
    GLint fill = -1;
    GLint gradientStops = -1;
    GLint gradientColors[4]{-1, -1, -1, -1};
    GLint gradientDirection = -1;
    GLint fillMode = -1;
    GLint radii = -1;
    GLint corners = -1;
    GLint insets = -1;
    GLint cornerPower = -1;
    GLint paintClip = -1;
    GLint paintClipRadius = -1;
    GLint paintClipPower = -1;
    GLint backdrop = -1;
    GLint copyBounds = -1;
    GLint backdropSize = -1;
    GLint displacementAxes = -1;
    GLint sampleRadius = -1;
    GLint parameters[8]{-1, -1, -1, -1, -1, -1, -1, -1};
    GLuint backdropTexture = 0;
    int backdropWidth = 0;
    int backdropHeight = 0;
  };

  struct Entry {
    Program active;
    std::string requestedDigest;
    std::string log;
    std::weak_ptr<const CustomEffectAsset> owner;
    std::uint32_t requestedAbi = 0;
  };

  [[nodiscard]] bool compile(Entry& entry, const CustomEffectAsset& asset);
  [[nodiscard]] bool drawActive(
      Program& program, const CustomEffectBinding& binding,
      float surfaceWidth, float surfaceHeight, float width, float height,
      const RoundedRectStyle& style, const Mat3& transform);

  std::unordered_map<std::string, Entry> m_entries;
  std::unordered_set<std::string> m_pendingRelease;
};
