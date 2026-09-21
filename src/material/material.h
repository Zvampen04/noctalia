#pragma once

// Standalone, palette-independent material descriptions. No Wayland, scene graph,
// GPU, JSON or preset dependency is allowed in this header.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace noctalia::material {

inline constexpr std::uint32_t kSchemaVersion = 4;
inline constexpr std::size_t kScalarFieldCount = 0
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) + 1
#include "fields.def"
#undef MATERIAL_FIELD
;

inline bool validScalarValue(std::string_view key, double value) noexcept {
  if (!std::isfinite(value)) return false;
  if (key=="optical_plane" && value!=-1.0 && value!=0.0 && value!=1.0) return false;
  if (key=="lens_mapping" && value!=0.0 && value!=1.0) return false;
#define MATERIAL_FIELD(member, storage, initial, low, high, step, label, group) \
  if (key==#storage) return static_cast<float>(value)>=low && static_cast<float>(value)<=high;
#include "fields.def"
#undef MATERIAL_FIELD
  return false;
}

enum class Primitive : std::uint8_t { Flat = 0, Plateau = 1, Optical = 2, Illustrated = 3 };

struct Vec2 {
  float x = 0.0F, y = 0.0F;
  bool operator==(const Vec2&) const = default;
};
struct Vec3 {
  float x = 0.0F, y = 0.0F, z = 1.0F;
  bool operator==(const Vec3&) const = default;
};

struct Lighting {
  // Surface-to-light vector in desktop coordinates: x right, y down, z outward.
  // Transform this vector into surface coordinates before shading rotated nodes.
  Vec3 direction{-0.55F, -0.55F, 0.63F};
  float diffuse = 0.9F;
  float highlight = 0.55F;
  float shadow = 0.65F;
  bool operator==(const Lighting&) const = default;
};

struct Plateau {
  float elevation = 3.5F;       // Logical pixels; negative means recessed.
  float shoulderWidth = 10.0F; // Logical pixels measured inward from the silhouette.
  float minimumFaceFraction = 0.40F; // Reserve this fraction of the shortest dimension.
  float contactRadius = 12.0F;
  float contactStrength = 0.25F; // Pair opacity, separate from color intensity.
  float shadowDistance = 4.0F; // Largest x/y component of the paired offsets.
  float shadowIntensity = 0.15F; // Multiplicative sRGB palette-color variation.
  float faceCurvature = 0.0F; // Negative concave, positive convex; zero flat.
  float faceHighlight = 0.07F;
  float faceShadow = 0.10F;
  bool operator==(const Plateau&) const = default;
};

struct Optical {
  float thickness = 14.0F;
  float edgeWidth = 18.0F;
  float refractiveIndex = 1.45F;
  float maximumDisplacement = 24.0F;
  float scatteringRadius = 1.5F;
  float tintOpacity = 0.08F;
  float rim = 0.6F;
  float specular = 0.32F;
  float sheen = 0.05F;
  float chromaticSeparation = 0.012F;
  float planeMode = -1.0F; // -1 semantic default, 0 inherited, 1 independent.
  float backdropBrightness = 0.0F;
  float backdropContrast = 1.0F;
  float backdropSaturation = 1.0F;
  float rimWidth = -1.0F; // -1 automatic; zero disables the rim band.
  float rimFalloff = 1.5F;
  float refractionRadius = -1.0F; // -1 follows painted shape; zero is a square lens field.
  float lensMapping = 0.0F; // 0 Snell, 1 bounded radial edge lens.
  float lensStrength = 0.2F;
  float lensFalloff = 1.0F;
  bool operator==(const Optical&) const = default;
};

struct Illustration {
  float strokeWidth = 1.2F;
  float strokeVariation = 0.28F;
  float strokeWavelength = 18.0F;
  float paintedEdgeWidth = 7.0F;
  float paintedStrength = 0.06F;
  // Stable component identity, never frame time or a repaint counter.
  std::uint32_t seed = 0;
  bool operator==(const Illustration&) const = default;
};

struct Parameters {
  Primitive primitive = Primitive::Flat;
  Lighting lighting{};
  Plateau plateau{};
  Optical optical{};
  Illustration illustration{};
  bool operator==(const Parameters&) const = default;
};

inline bool ownsOpticalPlane(const Parameters& p, bool semanticDefault) noexcept {
  return p.optical.planeMode==1.0F || (p.optical.planeMode==-1.0F && semanticDefault);
}

// Optional leaves preserve inheritance when only one material property changes.
// Apply patches in global -> semantic role -> control family -> surface -> state
// order. The caller owns role names, settings transactions and accessibility.
struct Patch {
  std::optional<Primitive> primitive;
  std::optional<Vec3> lightDirection;
  std::optional<float> diffuse, highlight, shadow;
  std::optional<float> elevation, shoulderWidth, minimumFaceFraction;
  std::optional<float> contactRadius, contactStrength;
  std::optional<float> shadowDistance, shadowIntensity, faceCurvature, faceHighlight, faceShadow;
  std::optional<float> thickness, edgeWidth, refractiveIndex, maximumDisplacement;
  std::optional<float> scatteringRadius, tintOpacity, rim, specular, sheen, chromaticSeparation;
  std::optional<float> planeMode, backdropBrightness, backdropContrast, backdropSaturation, rimWidth, rimFalloff;
  std::optional<float> refractionRadius, lensMapping, lensStrength, lensFalloff;
  std::optional<float> strokeWidth, strokeVariation, strokeWavelength, paintedEdgeWidth, paintedStrength;
  std::optional<std::uint32_t> seed;
};

namespace detail {
inline float finiteClamp(float value, float fallback, float low, float high) noexcept {
  return std::clamp(std::isfinite(value) ? value : fallback, low, high);
}
template <typename T> inline void assign(T& target, const std::optional<T>& source) {
  if (source) target = *source;
}
} // namespace detail

inline Parameters sanitize(Parameters p) noexcept {
  using detail::finiteClamp;
  if (static_cast<unsigned>(p.primitive) > static_cast<unsigned>(Primitive::Illustrated)) {
    p.primitive = Primitive::Flat;
  }
  auto& l = p.lighting;
  l.direction.x = finiteClamp(l.direction.x, -0.55F, -1.0F, 1.0F);
  l.direction.y = finiteClamp(l.direction.y, -0.55F, -1.0F, 1.0F);
  l.direction.z = finiteClamp(l.direction.z, 0.63F, 0.05F, 1.0F);
  const float length = std::sqrt(l.direction.x * l.direction.x + l.direction.y * l.direction.y
                               + l.direction.z * l.direction.z);
  l.direction = {l.direction.x / length, l.direction.y / length, l.direction.z / length};
  l.diffuse = finiteClamp(l.diffuse, 0.9F, 0.0F, 2.0F);
  l.highlight = finiteClamp(l.highlight, 0.55F, 0.0F, 1.0F);
  l.shadow = finiteClamp(l.shadow, 0.65F, 0.0F, 1.0F);
  auto& b = p.plateau;
  b.elevation = finiteClamp(b.elevation, 3.5F, -32.0F, 32.0F);
  b.shoulderWidth = finiteClamp(b.shoulderWidth, 10.0F, 0.0F, 128.0F);
  b.minimumFaceFraction = finiteClamp(b.minimumFaceFraction, 0.4F, 0.1F, 0.95F);
  b.contactRadius = finiteClamp(b.contactRadius, 12.0F, 0.0F, 128.0F);
  b.contactStrength = finiteClamp(b.contactStrength, 0.25F, 0.0F, 1.0F);
  b.shadowDistance = finiteClamp(b.shadowDistance, 4.0F, 0.0F, 64.0F);
  b.shadowIntensity = finiteClamp(b.shadowIntensity, 0.15F, 0.0F, 1.0F);
  b.faceCurvature = finiteClamp(b.faceCurvature, 0.0F, -1.0F, 1.0F);
  b.faceHighlight = finiteClamp(b.faceHighlight, 0.07F, 0.0F, 1.0F);
  b.faceShadow = finiteClamp(b.faceShadow, 0.10F, 0.0F, 1.0F);
  auto& o = p.optical;
  o.thickness = finiteClamp(o.thickness, 14.0F, 0.0F, 128.0F);
  o.edgeWidth = finiteClamp(o.edgeWidth, 18.0F, 0.0F, 128.0F);
  o.refractiveIndex = finiteClamp(o.refractiveIndex, 1.45F, 1.0F, 2.5F);
  o.maximumDisplacement = finiteClamp(o.maximumDisplacement, 24.0F, 0.0F, 128.0F);
  o.scatteringRadius = finiteClamp(o.scatteringRadius, 1.5F, 0.0F, 24.0F);
  o.tintOpacity = finiteClamp(o.tintOpacity, 0.08F, 0.0F, 1.0F);
  o.rim = finiteClamp(o.rim, 0.6F, 0.0F, 1.0F);
  o.specular = finiteClamp(o.specular, 0.32F, 0.0F, 1.0F);
  o.sheen = finiteClamp(o.sheen, 0.05F, 0.0F, 1.0F);
  o.chromaticSeparation = finiteClamp(o.chromaticSeparation, 0.012F, 0.0F, 0.1F);
  if (!validScalarValue("optical_plane",o.planeMode)) o.planeMode=-1.0F;
  o.backdropBrightness=finiteClamp(o.backdropBrightness,0.0F,-1.0F,1.0F);
  o.backdropContrast=finiteClamp(o.backdropContrast,1.0F,0.0F,2.0F);
  o.backdropSaturation=finiteClamp(o.backdropSaturation,1.0F,0.0F,3.0F);
  o.rimWidth=finiteClamp(o.rimWidth,-1.0F,-1.0F,128.0F);
  o.rimFalloff=finiteClamp(o.rimFalloff,1.5F,0.0F,8.0F);
  o.refractionRadius=finiteClamp(o.refractionRadius,-1.0F,-1.0F,512.0F);
  if (!validScalarValue("lens_mapping",o.lensMapping)) o.lensMapping=0.0F;
  o.lensStrength=finiteClamp(o.lensStrength,0.2F,0.0F,1.0F);
  o.lensFalloff=finiteClamp(o.lensFalloff,1.0F,0.1F,16.0F);
  auto& i = p.illustration;
  i.strokeWidth = finiteClamp(i.strokeWidth, 1.2F, 0.0F, 8.0F);
  i.strokeVariation = finiteClamp(i.strokeVariation, 0.28F, 0.0F, 1.5F);
  i.strokeWavelength = finiteClamp(i.strokeWavelength, 18.0F, 4.0F, 256.0F);
  i.paintedEdgeWidth = finiteClamp(i.paintedEdgeWidth, 7.0F, 0.0F, 64.0F);
  i.paintedStrength = finiteClamp(i.paintedStrength, 0.06F, 0.0F, 0.25F);
  return p;
}

inline Parameters resolve(Parameters base, std::span<const Patch> patches) noexcept {
  using detail::assign;
  for (const auto& patch : patches) {
    assign(base.primitive, patch.primitive);
    assign(base.lighting.direction, patch.lightDirection);
    assign(base.lighting.diffuse, patch.diffuse);
    assign(base.lighting.highlight, patch.highlight);
    assign(base.lighting.shadow, patch.shadow);
    assign(base.plateau.elevation, patch.elevation);
    assign(base.plateau.shoulderWidth, patch.shoulderWidth);
    assign(base.plateau.minimumFaceFraction, patch.minimumFaceFraction);
    assign(base.plateau.contactRadius, patch.contactRadius);
    assign(base.plateau.contactStrength, patch.contactStrength);
    assign(base.plateau.shadowDistance, patch.shadowDistance);
    assign(base.plateau.shadowIntensity, patch.shadowIntensity);
    assign(base.plateau.faceCurvature, patch.faceCurvature);
    assign(base.plateau.faceHighlight, patch.faceHighlight);
    assign(base.plateau.faceShadow, patch.faceShadow);
    assign(base.optical.thickness, patch.thickness);
    assign(base.optical.edgeWidth, patch.edgeWidth);
    assign(base.optical.refractiveIndex, patch.refractiveIndex);
    assign(base.optical.maximumDisplacement, patch.maximumDisplacement);
    assign(base.optical.scatteringRadius, patch.scatteringRadius);
    assign(base.optical.tintOpacity, patch.tintOpacity);
    assign(base.optical.rim, patch.rim);
    assign(base.optical.specular, patch.specular);
    assign(base.optical.sheen, patch.sheen);
    assign(base.optical.chromaticSeparation, patch.chromaticSeparation);
    assign(base.optical.planeMode, patch.planeMode);
    assign(base.optical.backdropBrightness, patch.backdropBrightness);
    assign(base.optical.backdropContrast, patch.backdropContrast);
    assign(base.optical.backdropSaturation, patch.backdropSaturation);
    assign(base.optical.rimWidth, patch.rimWidth);
    assign(base.optical.rimFalloff, patch.rimFalloff);
    assign(base.optical.refractionRadius, patch.refractionRadius);
    assign(base.optical.lensMapping, patch.lensMapping);
    assign(base.optical.lensStrength, patch.lensStrength);
    assign(base.optical.lensFalloff, patch.lensFalloff);

    assign(base.illustration.strokeWidth, patch.strokeWidth);
    assign(base.illustration.strokeVariation, patch.strokeVariation);
    assign(base.illustration.strokeWavelength, patch.strokeWavelength);
    assign(base.illustration.paintedEdgeWidth, patch.paintedEdgeWidth);
    assign(base.illustration.paintedStrength, patch.paintedStrength);
    assign(base.illustration.seed, patch.seed);
  }
  return sanitize(base);
}

// Call with the actual drawable body size after logical insets, before upload.
// The ramps cannot consume the flat face, even on narrow controls or small views.
inline Parameters fitToBounds(Parameters p, float width, float height) noexcept {
  p = sanitize(p);
  const float extent = std::max(0.0F, std::min(std::isfinite(width) ? width : 0.0F,
                                           std::isfinite(height) ? height : 0.0F));
  const float shoulderLimit = extent * (1.0F - p.plateau.minimumFaceFraction) * 0.5F;
  p.plateau.shoulderWidth = std::min(p.plateau.shoulderWidth, shoulderLimit);
  // A subpixel shoulder cannot plausibly support a tall extrusion. Bound slope
  // as controls shrink; avoid a new hard vertical side at the minimum size.
  const float elevationLimit = p.plateau.shoulderWidth * 0.75F;
  if (extent <= 0.0F) p.plateau.elevation = 0.0F;
  else if (p.plateau.shoulderWidth > 0.0001F)
    p.plateau.elevation = std::clamp(p.plateau.elevation, -elevationLimit, elevationLimit);
  p.optical.edgeWidth = std::min(p.optical.edgeWidth, extent * 0.5F);
  p.illustration.strokeWidth = std::min(p.illustration.strokeWidth, extent * 0.25F);
  p.illustration.strokeVariation = std::min(p.illustration.strokeVariation,
                                          p.illustration.strokeWidth * 0.45F);
  p.illustration.paintedEdgeWidth = std::min(p.illustration.paintedEdgeWidth, extent * 0.5F);
  return p;
}

// CPU reference for the GLSL height field. SDF is negative inside the shape.
// A quintic ramp has zero first AND second derivatives at both seams.
inline float plateauHeight(float distance, float shoulder, float elevation) noexcept {
  if (!(shoulder > 0.0001F)) return 0.0F;
  const float t = std::clamp(-distance / shoulder, 0.0F, 1.0F);
  return elevation * t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
}
inline float plateauDerivative(float distance, float shoulder, float elevation) noexcept {
  if (!(shoulder > 0.0001F)) return 0.0F;
  const float t = std::clamp(-distance / shoulder, 0.0F, 1.0F);
  return -elevation * 30.0F * t * t * (t - 1.0F) * (t - 1.0F) / shoulder;
}

// The generator's multiplicative sRGB face treatment, without a palette choice.
inline float plateauFaceChannel(float color, float position, const Plateau& p) noexcept {
  const float t = std::clamp(position, 0.0F, 1.0F);
  const float first = p.faceCurvature >= 0.0F ? p.faceHighlight : -p.faceShadow;
  const float last = p.faceCurvature >= 0.0F ? -p.faceShadow : p.faceHighlight;
  const float a = std::clamp(color * (1.0F + first), 0.0F, 1.0F);
  const float b = std::clamp(color * (1.0F + last), 0.0F, 1.0F);
  return std::lerp(color, std::lerp(a, b, t), std::abs(p.faceCurvature));
}
inline Vec2 plateauShadowOffset(const Parameters& p) noexcept {
  const auto& light = p.lighting.direction;
  const float extent = std::max(std::abs(light.x), std::abs(light.y));
  if (extent <= 0.0001F) return {};
  return {-light.x / extent * p.plateau.shadowDistance, -light.y / extent * p.plateau.shadowDistance};
}

// CPU reference of the shared GLSL smooth displacement bound.
inline Vec2 smoothOpticalLimit(Vec2 offset, float limit) noexcept {
  if (limit <= 0.0F) return {};
  const float magnitude = std::hypot(offset.x, offset.y);
  const float scale = std::max({magnitude, limit, 0.0001F});
  const float ratio = std::min(magnitude, limit) / scale;
  const float ratio2 = ratio * ratio;
  const float factor = (limit / scale) / std::pow(1.0F + ratio2 * ratio2, 0.25F);
  return {offset.x * factor, offset.y * factor};
}

inline Vec2 radialLensVector(Vec2 fromCenter, Vec2 halfSize) noexcept {
  const float scale = std::max({std::abs(fromCenter.x), std::abs(fromCenter.y), 0.0001F});
  const float x = fromCenter.x / scale, y = fromCenter.y / scale;
  const float magnitude = std::max(std::hypot(x, y), 0.0001F);
  return {x / magnitude * std::max(halfSize.x, 0.0F),
          y / magnitude * std::max(halfSize.y, 0.0F)};
}

// C2-continuous face-to-edge transition for every supported falloff, including
// values below one. The rational bias preserves the strength/width controls.
inline float radialLensEnvelope(float proximity, float falloff) noexcept {
  const float p = std::clamp(proximity, 0.0F, 1.0F);
  falloff = std::max(falloff, 0.1F);
  const float t = p / (falloff + (1.0F - falloff) * p);
  const float u = std::min(t, 1.0F - t);
  const float blend = u * u * u * (10.0F + u * (-15.0F + 6.0F * u));
  const float shoulder = t <= 0.5F ? blend : 1.0F - blend;
  constexpr float epsilon = 0.02F;
  const float base = std::sqrt(1.0F + epsilon * epsilon);
  return shoulder * shoulder * (base + epsilon)
      / (base + std::sqrt(std::max(1.0F - shoulder * shoulder, 0.0F) + epsilon * epsilon));
}

// Normal-incidence ray entering glass from air, with a bounded screen offset.
inline Vec2 opticalDisplacement(Vec2 slope, float thickness, float refractiveIndex,
                                float maximumDisplacement) noexcept {
  Optical o;
  o.thickness = thickness;
  o.refractiveIndex = refractiveIndex;
  o.maximumDisplacement = maximumDisplacement;
  Parameters p;
  p.optical = o;
  o = sanitize(p).optical;
  slope.x = detail::finiteClamp(slope.x, 0.0F, -64.0F, 64.0F);
  slope.y = detail::finiteClamp(slope.y, 0.0F, -64.0F, 64.0F);
  const float invLength = 1.0F / std::sqrt(slope.x * slope.x + slope.y * slope.y + 1.0F);
  const Vec3 normal{-slope.x * invLength, -slope.y * invLength, invLength};
  const float eta = 1.0F / o.refractiveIndex;
  const float dot = -normal.z;
  const float factor = eta * dot + std::sqrt(std::max(0.0F, 1.0F - eta * eta * (1.0F - dot * dot)));
  const float rayZ = -eta - factor * normal.z;
  Vec2 offset{-factor * normal.x * o.thickness / std::max(-rayZ, 0.15F),
              -factor * normal.y * o.thickness / std::max(-rayZ, 0.15F)};
  return smoothOpticalLimit(offset, o.maximumDisplacement);
}

// Stable phase without reducing large IDs through a lossy uint -> float cast.
inline float illustrationPhase(std::uint32_t seed) noexcept {
  seed ^= seed >> 16U;
  seed *= 0x7feb352dU;
  seed ^= seed >> 15U;
  seed *= 0x846ca68bU;
  seed ^= seed >> 16U;
  return static_cast<float>(seed & 0xffffU) * (6.28318530718F / 65536.0F);
}

// Effect bounds in logical pixels. Transform into device space and round outward
// before allocating textures or damage. Optical padding includes chromatic taps.
inline float samplingPadding(const Parameters& parameters) noexcept {
  const auto p = sanitize(parameters);
  if (p.primitive == Primitive::Optical) {
    return std::ceil(p.optical.maximumDisplacement * (1.0F + p.optical.chromaticSeparation)
                     + p.optical.scatteringRadius + 2.0F);
  }
  if (p.primitive == Primitive::Plateau && p.plateau.elevation >= 0.0F) {
    return std::ceil(p.plateau.contactRadius * 1.5F + p.plateau.shadowDistance + 2.0F);
  }
  return 1.0F;
}

// Reference CPU evaluator for the optional radial mapping, in logical pixels.
// The caller supplies the optical SDF; final painted coverage stays separate.
inline Vec2 radialOpticalDisplacement(Vec2 fromCenter, float distance, const Optical& input) noexcept {
  Parameters parameters; parameters.optical=input;
  const auto o=sanitize(parameters).optical;
  if (!std::isfinite(fromCenter.x) || !std::isfinite(fromCenter.y) || !std::isfinite(distance)
      || o.edgeWidth<=0.0001F || o.maximumDisplacement<=0.0F || o.lensStrength<=0.0F) return {};
  const float proximity=std::clamp(1.0F+distance/o.edgeWidth,0.0F,1.0F);
  const float amount=o.lensStrength*radialLensEnvelope(proximity,o.lensFalloff);
  const float x=-fromCenter.x*amount, y=-fromCenter.y*amount;
  return smoothOpticalLimit({x,y},o.maximumDisplacement);
}

// Uniforms are plain vec4 packs for both GLES 2 and desktop GL. Upload values
// after fitToBounds(); the GLSL helpers have no hidden global uniform dependency.
struct Uniforms {
  int primitive = 0;
  std::array<float, 4> light{};        // direction xyz, diffuse
  std::array<float, 4> plateau{};      // elevation, shoulder, highlight, shadow
  std::array<float, 4> contact{};      // radius, strength, minimum face fraction, reserved
  std::array<float, 4> plateauShape{}; // shadow distance, intensity, face curvature, reserved
  std::array<float, 4> plateauFace{}; // face highlight, face shadow, reserved, reserved
  std::array<float, 4> optical{};      // thickness, edge width, refractive index, max displacement
  std::array<float, 4> opticalStyle{}; // scatter radius, tint opacity, chromatic separation, rim falloff
  std::array<float, 4> opticalLight{}; // rim, specular, sheen, rim width
  std::array<float, 4> opticalColor{}; // backdrop brightness, contrast, saturation, reserved
  std::array<float, 4> opticalLens{}; // optical radius, mapping, radial strength, radial falloff
  std::array<float, 4> illustration{}; // stroke width, variation, wavelength, stable phase
  std::array<float, 4> paint{};        // edge width, strength, reserved, reserved
};

inline Uniforms uniforms(const Parameters& p) noexcept {
  return {static_cast<int>(p.primitive),
          {p.lighting.direction.x, p.lighting.direction.y, p.lighting.direction.z, p.lighting.diffuse},
          {p.plateau.elevation, p.plateau.shoulderWidth, p.lighting.highlight, p.lighting.shadow},
          {p.plateau.contactRadius, p.plateau.contactStrength, p.plateau.minimumFaceFraction, 0.0F},
          {p.plateau.shadowDistance, p.plateau.shadowIntensity, p.plateau.faceCurvature, 0.0F},
          {p.plateau.faceHighlight, p.plateau.faceShadow, 0.0F, 0.0F},
          {p.optical.thickness, p.optical.edgeWidth, p.optical.refractiveIndex, p.optical.maximumDisplacement},
          {p.optical.scatteringRadius, p.optical.tintOpacity, p.optical.chromaticSeparation, p.optical.rimFalloff},
          {p.optical.rim, p.optical.specular, p.optical.sheen, p.optical.rimWidth},
          {p.optical.backdropBrightness,p.optical.backdropContrast,p.optical.backdropSaturation,0.0F},
          {p.optical.refractionRadius,p.optical.lensMapping,p.optical.lensStrength,p.optical.lensFalloff},
          {p.illustration.strokeWidth, p.illustration.strokeVariation, p.illustration.strokeWavelength,
           illustrationPhase(p.illustration.seed)},
          {p.illustration.paintedEdgeWidth, p.illustration.paintedStrength, 0.0F, 0.0F}};
}

} // namespace noctalia::material
