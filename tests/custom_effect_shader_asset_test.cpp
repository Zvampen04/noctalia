#include "render/custom_effect/custom_effect_types.h"

#include <cassert>
#include <string>

namespace {
CustomEffectAsset validAsset() {
  return CustomEffectAsset{
      .stableId = "user.edge-sheen",
      .sha256Digest = std::string(64, 'a'),
      .source = R"(
vec4 noctalia_effect(vec4 s,vec4 b,vec2 uv,vec2 px,vec2 size,
  vec4 p0,vec4 p1,vec4 p2,vec4 p3,vec4 p4,vec4 p5,vec4 p6,vec4 p7) {
  return vec4(s.rgb/max(s.a,0.0001),1.0);
})",
      .maxSampleRadiusPx = 12.0F,
  };
}
}

int main() {
  std::string error;
  auto asset = validAsset();
  assert(validCustomEffectAsset(asset, &error));
  assert(error.empty());

  auto invalid = asset;
  invalid.source = "void main() {}";
  assert(!validCustomEffectAsset(invalid, &error));
  assert(!error.empty());

  invalid = asset;
  invalid.source = "vec4 noctalia_effect(){return texture2D(secret,vec2(0.0));}";
  assert(!validCustomEffectAsset(invalid));
  invalid=asset;
  invalid.source="// uniform sampler texture are harmless here\n"+invalid.source+"/* gl_ main */";
  assert(validCustomEffectAsset(invalid,&error));
  invalid.source+="/* unterminated";
  assert(!validCustomEffectAsset(invalid,&error));
  assert(error.find("unterminated")!=std::string::npos);
  invalid = asset;
  invalid.source.assign(kCustomEffectMaxSourceBytes + 1, 'x');
  assert(!validCustomEffectAsset(invalid));
  invalid = asset;
  invalid.maxSampleRadiusPx = kCustomEffectMaxSampleRadiusPx + 1.0F;
  assert(!validCustomEffectAsset(invalid));
  invalid = asset;
  invalid.sha256Digest[0] = 'A';
  assert(!validCustomEffectAsset(invalid));

  // An invalid replacement is rejected before the renderer cache sees it; the
  // cache therefore keeps its last-good program or draws the native fallback.
  assert(validCustomEffectAsset(asset));

  auto firstTarget=CustomEffectBinding{.asset=std::make_shared<CustomEffectAsset>(asset)};
  auto secondTarget=firstTarget;
  firstTarget.parameters[0][0]=2.0F;
  secondTarget.parameters[0][0]=9.0F;
  assert(firstTarget.asset==secondTarget.asset);
  assert(firstTarget.parameters!=secondTarget.parameters);

  auto nextAbi=asset;
  nextAbi.abi=kCustomEffectAbiVersion+1;
  assert(!validCustomEffectAsset(nextAbi));
  return 0;
}
