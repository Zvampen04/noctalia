#include "render/custom_effect/custom_effect_consumers.h"

#include <cassert>

int main() {
  CustomEffectConsumers consumers;
  using Effects = CustomEffectConsumers::EffectSet;
  constexpr CustomEffectConsumers::SurfaceId bar = 1;
  constexpr CustomEffectConsumers::SurfaceId popup = 2;

  assert(consumers.update(bar, Effects{"glass", "rim"}).empty());
  assert(consumers.update(popup, Effects{"glass"}).empty());
  assert(consumers.references("glass") == 2);
  // A failed/aborted frame performs no update and preserves the last complete
  // scene. Removing an unknown owner likewise cannot disturb live references.
  assert(consumers.remove(99).empty());
  assert(consumers.references("glass") == 2);

  // One surface dropping an effect cannot release a sibling's program.
  assert(consumers.update(bar, Effects{"rim"}).empty());
  assert(consumers.references("glass") == 1);
  assert(consumers.update(popup, Effects{"glass"}).empty());

  auto released = consumers.remove(popup);
  assert(released.size() == 1 && released[0] == "glass");
  assert(consumers.references("rim") == 1);
  released = consumers.remove(bar);
  assert(released.size() == 1 && released[0] == "rim");
  assert(consumers.surfaces() == 0);

  // Closing and reopening one sibling surface cannot churn a shader still
  // referenced by another surface. Only the final live scene releases it.
  assert(consumers.update(bar, Effects{"shared"}).empty());
  assert(consumers.update(popup, Effects{"shared"}).empty());
  assert(consumers.remove(bar).empty());
  assert(consumers.references("shared") == 1);
  assert(consumers.update(bar, Effects{"shared"}).empty());
  assert(consumers.references("shared") == 2);
  assert(consumers.remove(popup).empty());
  assert(consumers.references("shared") == 1);
  released = consumers.remove(bar);
  assert(released.size() == 1 && released[0] == "shared");
  assert(consumers.references("shared") == 0 && consumers.surfaces() == 0);

  // A configured but absent effect is never registered by this scene API.
  assert(consumers.references("configured-but-absent") == 0);
}
