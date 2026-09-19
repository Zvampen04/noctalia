#include "custom_effect_lease.h"

#include <cstdlib>
#include <iostream>

using namespace noctalia::material;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; std::exit(1); } } while (false)

namespace {
SceneDescriptor scene() {
  SceneDescriptor result{800, 600, {}};
  ScenePlane plane;
  plane.group = 41;
  plane.role = "widget";
  plane.surface = "bar.default.project1-status.cpu-primary";
  plane.width = 160;
  plane.height = 48;
  plane.clip = {0, 0, 800, 600};
  plane.customEffect = SceneCustomEffect{};
  plane.customEffect->transportDigest.fill(0x31);
  result.planes.push_back(plane);
  return result;
}
}

int main() {
  auto base = scene();
  const auto signature = customEffectLeaseSignature(base);
  CHECK(signature.size() == 1);
  CHECK(!validCustomEffectLeaseScene(base, 7, signature));
  base.leaseToken = 7;
  CHECK(validCustomEffectLeaseScene(base, 7, signature));

  auto animated = base;
  animated.planes[0].transform[4] = 123;
  animated.planes[0].opacity = 0;
  animated.planes[0].tint = {0.2F, 0.4F, 0.6F, 0};
  animated.planes[0].customEffect->sampleRadius = 128;
  animated.planes[0].customEffect->parameters[7][3] = -16;
  CHECK(customEffectLeaseSignature(animated) == signature);
  CHECK(validCustomEffectLeaseScene(animated, 7, signature));

  for (auto changed : {base, base, base, base, base}) {
    static int field = 0;
    switch (field++) {
    case 0: changed.planes[0].group++; break;
    case 1: changed.planes[0].role = "popup"; break;
    case 2: changed.planes[0].surface = "bar.default.project1-status.cpu-secondary"; break;
    case 3: changed.planes[0].customEffect->transportDigest[0]++; break;
    case 4: changed.planes[0].customEffect->abi++; break;
    }
    CHECK(customEffectLeaseSignature(changed) != signature);
    CHECK(!validCustomEffectLeaseScene(changed, 7, signature));
  }

  auto second = base.planes[0];
  second.group = 42;
  second.surface = "bar.default.project1-status.ram-primary";
  base.planes.push_back(second);
  CHECK(customEffectLeaseSignature(base) != signature);
  CHECK(!validCustomEffectLeaseScene(base, 7, signature));
  CHECK(!validCustomEffectLeaseScene(animated, 0, signature));
  std::cout << "custom effect lease signature and bounded animation fields passed\n";
}
