#include "material.h"
#include "shader_source.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>

using namespace noctalia::material;

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
  std::exit(EXIT_FAILURE); } } while (false)

static bool near(float a, float b, float tolerance = 0.0001F) {
  return std::abs(a - b) <= tolerance;
}

int main() {
  CHECK(kSchemaVersion==4 && kScalarFieldCount==41);
  for (double value:{0.0,1.0}) CHECK(validScalarValue("lens_mapping",value));
  for (double value:{-1.0,-.5,.5,.999999999,1.000000001,1e-300}) CHECK(!validScalarValue("lens_mapping",value));
  for (double value:{-1.0,0.0,1.0}) CHECK(validScalarValue("optical_plane",value));
  for (double value:{-.5,.5,.999999999,1.000000001,1e-300}) CHECK(!validScalarValue("optical_plane",value));
  CHECK(!validScalarValue("unknown",0));
  CHECK(validScalarValue("rim_width",0));
  CHECK(validScalarValue("rim_width",-1));
  Parameters ownership;
  CHECK(ownsOpticalPlane(ownership,true));CHECK(!ownsOpticalPlane(ownership,false));
  ownership.optical.planeMode=1;CHECK(ownsOpticalPlane(ownership,false));
  ownership.optical.planeMode=0;CHECK(!ownsOpticalPlane(ownership,true));
  ownership.optical.planeMode=.5F;CHECK(sanitize(ownership).optical.planeMode==-1);
  Patch optics;optics.planeMode=1;optics.rimWidth=0;optics.backdropBrightness=.2F;
  const auto customized=resolve(Parameters{},std::array{optics});
  CHECK(customized.optical.planeMode==1 && customized.optical.rimWidth==0);
  CHECK(uniforms(customized).opticalColor[0]==.2F && uniforms(customized).opticalLight[3]==0);
  Patch lensPatch;lensPatch.refractionRadius=0;lensPatch.lensMapping=1;lensPatch.lensStrength=0;lensPatch.lensFalloff=3;
  const auto lensOverride=resolve(customized,std::array{lensPatch});
  CHECK(lensOverride.optical.refractionRadius==0 && lensOverride.optical.lensStrength==0);
  CHECK((uniforms(lensOverride).opticalLens==std::array<float,4>{0,1,0,3}));
  CHECK((uniforms(Parameters{}).opticalLens==std::array<float,4>{-1,0,.2F,1}));
  auto invalidLens=Parameters{};invalidLens.optical.lensMapping=.5F;
  CHECK(sanitize(invalidLens).optical.lensMapping==0);

  // Flat means a region, not just a zero-slope point at a bubble's center.
  for (float distance : {-10.0F, -11.0F, -30.0F, -100.0F}) {
    CHECK(near(plateauHeight(distance, 10.0F, 4.0F), 4.0F));
    CHECK(near(plateauDerivative(distance, 10.0F, 4.0F), 0.0F));
  }
  CHECK(plateauHeight(1.0F, 10.0F, 4.0F) == 0.0F);
  CHECK(plateauDerivative(0.0F, 10.0F, 4.0F) == 0.0F);
  CHECK(plateauHeight(-5.0F, 0.0F, 4.0F) == 0.0F);
  CHECK(plateauDerivative(-5.0F, 0.0F, 4.0F) == 0.0F);

  // Continuous shoulders, monotone rise, inverse inset profile and normal slope.
  float previous = 0.0F;
  for (int step = 1; step <= 100; ++step) {
    const float d = -0.1F * static_cast<float>(step);
    const float h = plateauHeight(d, 10.0F, 4.0F);
    CHECK(h >= previous - 0.00001F);
    CHECK(h <= 4.00001F);
    CHECK(near(h, -plateauHeight(d, 10.0F, -4.0F)));
    const float numerical = (plateauHeight(d + 0.005F, 10.0F, 4.0F)
                            - plateauHeight(d - 0.005F, 10.0F, 4.0F)) / 0.01F;
    CHECK(near(numerical, plateauDerivative(d, 10.0F, 4.0F), 0.002F));
    previous = h;
  }
  CHECK(std::abs(plateauDerivative(-0.01F, 10.0F, 4.0F)) < 0.0001F);
  CHECK(std::abs(plateauDerivative(-9.99F, 10.0F, 4.0F)) < 0.0001F);

  // A small component still has a visible flat face and cannot gain vertical walls.
  Parameters source;
  source.primitive = Primitive::Plateau;
  source.plateau.shoulderWidth = 80.0F;
  source.plateau.elevation = 20.0F;
  const auto narrow = fitToBounds(source, 80.0F, 20.0F);
  CHECK(near(narrow.plateau.shoulderWidth, 6.0F));
  CHECK(20.0F - 2.0F * narrow.plateau.shoulderWidth >= 8.0F);
  CHECK(narrow.plateau.elevation <= narrow.plateau.shoulderWidth * 0.75F);
  const auto empty = fitToBounds(source, 0.0F, 100.0F);
  CHECK(empty.plateau.shoulderWidth == 0.0F && empty.plateau.elevation == 0.0F);

  // A state override changes only its requested leaves, including explicit zero.
  Patch role;
  role.elevation = 5.0F;
  role.shoulderWidth = 12.0F;
  role.refractiveIndex = 1.8F;
  Patch pressed;
  pressed.elevation = -2.0F;
  pressed.tintOpacity = 0.0F;
  const std::array patches{role, pressed};
  const auto inherited = resolve(source, patches);
  CHECK(inherited.plateau.elevation == -2.0F);
  CHECK(inherited.plateau.shoulderWidth == 12.0F);
  CHECK(inherited.optical.refractiveIndex == 1.8F);
  CHECK(inherited.optical.tintOpacity == 0.0F);
  CHECK(inherited.primitive == Primitive::Plateau);
  CHECK(source.plateau.elevation == 20.0F);

  // Invalid imported data must not result in NaN uniforms or unbounded allocations.
  Parameters invalid;
  invalid.primitive = static_cast<Primitive>(255);
  invalid.lighting.direction = {0.0F, 0.0F, 0.0F};
  invalid.plateau.shoulderWidth = std::numeric_limits<float>::quiet_NaN();
  invalid.optical.maximumDisplacement = std::numeric_limits<float>::infinity();
  invalid.optical.refractiveIndex = -1.0F;
  invalid.illustration.strokeVariation = 500.0F;
  const auto safe = fitToBounds(invalid, 200.0F, 20.0F);
  CHECK(safe.primitive == Primitive::Flat);
  CHECK(near(safe.lighting.direction.z, 1.0F));
  CHECK(std::isfinite(safe.plateau.shoulderWidth));
  CHECK(safe.optical.maximumDisplacement <= 128.0F);
  CHECK(safe.optical.refractiveIndex == 1.0F);
  CHECK(safe.illustration.strokeVariation < safe.illustration.strokeWidth * 0.5F);

  // The four shapes keep face curvature independent of outer/inset elevation.
  Parameters shapes;shapes.primitive=Primitive::Plateau;
  shapes.plateau.shoulderWidth=0;shapes.plateau.shadowDistance=20;
  shapes.plateau.contactRadius=60;shapes.plateau.contactStrength=1;
  for(float curvature:{-1.F,0.F,1.F}) {
    shapes.plateau.faceCurvature=curvature;
    const float start=plateauFaceChannel(.8F,0,shapes.plateau);
    const float end=plateauFaceChannel(.8F,1,shapes.plateau);
    CHECK(curvature<0 ? start<end : curvature>0 ? start>end : start==end);
    CHECK(near(plateauFaceChannel(0.F,0,shapes.plateau),0.F));
  }
  shapes.plateau.faceCurvature=-1;
  CHECK(near(plateauFaceChannel(.8F,0,shapes.plateau),.72F));
  CHECK(near(plateauFaceChannel(.8F,1,shapes.plateau),.856F));
  auto offset=plateauShadowOffset(shapes);CHECK(near(offset.x,20.F)&&near(offset.y,20.F));
  const auto extentBefore=samplingPadding(shapes);
  shapes.plateau.elevation=20;CHECK(samplingPadding(shapes)==extentBefore);
  shapes.plateau.elevation=-3;CHECK(fitToBounds(shapes,100,60).plateau.elevation==-3);
  shapes.plateau.faceCurvature=0;CHECK(plateauFaceChannel(.8F,.1F,shapes.plateau)==.8F);
  shapes.plateau.shadowDistance=0;CHECK(plateauShadowOffset(shapes)==Vec2{});
  const auto shapePacks=uniforms(shapes);CHECK(shapePacks.plateauShape[2]==0);
  CHECK(shapePacks.plateauFace[0]==.07F&&shapePacks.contact[0]==60.F);

  // Normal incidence / no material / zero thickness cannot displace the backdrop.
  CHECK(opticalDisplacement({0.0F, 0.0F}, 24.0F, 1.45F, 20.0F) == Vec2{});
  const auto noIndex = opticalDisplacement({1.0F, -1.0F}, 24.0F, 1.0F, 20.0F);
  CHECK(near(noIndex.x, 0.0F) && near(noIndex.y, 0.0F));
  CHECK(opticalDisplacement({1.0F, 1.0F}, 0.0F, 1.45F, 20.0F) == Vec2{});
  for (const float slope : {0.1F, 1.0F, 10.0F, 1000.0F}) {
    const auto positive = opticalDisplacement({slope, slope * 0.5F}, 128.0F, 2.5F, 12.0F);
    const auto negative = opticalDisplacement({-slope, -slope * 0.5F}, 128.0F, 2.5F, 12.0F);
    CHECK(std::hypot(positive.x, positive.y) <= 12.0001F);
    CHECK(near(positive.x, -negative.x) && near(positive.y, -negative.y));
  }
  Parameters glass;
  glass.primitive = Primitive::Optical;
  glass.optical.maximumDisplacement = 20.0F;
  glass.optical.chromaticSeparation = 0.1F;
  glass.optical.scatteringRadius = 3.0F;
  CHECK(samplingPadding(glass) >= 25.0F);

  // Radial lens uses local logical vectors, never normalized screen UVs. Its
  // transverse component on a wide bar is intentionally distinct from Snell.
  Optical radial;radial.edgeWidth=20;radial.lensStrength=.2F;radial.maximumDisplacement=128;
  const auto topLeft=radialOpticalDisplacement({-80,-35},0,radial);
  CHECK(near(topLeft.x,16) && near(topLeft.y,7));
  CHECK(radialOpticalDisplacement({0,0},0,radial)==Vec2{});
  CHECK(radialOpticalDisplacement({-80,-35},-20,radial)==Vec2{});
  const auto opposite=radialOpticalDisplacement({80,35},0,radial);
  CHECK(near(opposite.x,-topLeft.x) && near(opposite.y,-topLeft.y));
  for (float exponent:{.1F,1.F,16.F}) for (float distance:{-100.F,-20.F,-10.F,-.01F,0.F,20.F}) {
    radial.lensFalloff=exponent;radial.maximumDisplacement=3;
    const auto offset=radialOpticalDisplacement({-10000,5000},distance,radial);
    CHECK(std::isfinite(offset.x) && std::isfinite(offset.y));
    CHECK(std::hypot(offset.x,offset.y)<=3.0001F);
  }
  radial.edgeWidth=0;CHECK(radialOpticalDisplacement({-80,-35},0,radial)==Vec2{});
  radial.edgeWidth=20;radial.lensStrength=0;CHECK(radialOpticalDisplacement({-80,-35},0,radial)==Vec2{});
  radial.lensStrength=1;radial.maximumDisplacement=0;CHECK(radialOpticalDisplacement({-80,-35},0,radial)==Vec2{});

  // Stable identity is deterministic, bounded, and independent of repaint time.
  CHECK(illustrationPhase(0x12345678U) == illustrationPhase(0x12345678U));
  CHECK(illustrationPhase(0x12345678U) != illustrationPhase(0x12345679U));
  CHECK(illustrationPhase(0xffffffffU) >= 0.0F && illustrationPhase(0xffffffffU) < 6.283186F);
  const auto upload = uniforms(narrow);
  CHECK(upload.plateau[0] == narrow.plateau.elevation);
  CHECK(upload.plateau[1] == narrow.plateau.shoulderWidth);
  CHECK(std::string_view{kMaterialShaderSource}.find("uniform ") == std::string_view::npos);

  // Registry ranges/defaults stay synchronized with the C++ type and sanitizer.
  const Parameters defaults;
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) \
  CHECK(defaults.member == initial); \
  if (!std::string_view{#member}.starts_with("lighting.direction")) { \
    auto p = defaults; p.member = low - 1000.0F; CHECK(sanitize(p).member >= low); \
    p.member = high + 1000.0F; CHECK(sanitize(p).member <= high); }
#include "fields.def"
#undef MATERIAL_FIELD

  std::puts("material geometry, inheritance, optics and registry checks passed");
}
