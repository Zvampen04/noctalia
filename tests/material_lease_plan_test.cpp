#include "wayland/material_lease_plan.h"

#include <cassert>
#include <utility>

namespace {
noctalia::material::CustomEffectLeaseSignature signature(std::string surface = "bar.default.cpu") {
  noctalia::material::CustomEffectLeaseTarget target;
  target.group = 7;
  target.role = "widget";
  target.surface = std::move(surface);
  target.transportDigest.fill(0x31);
  target.abi = 1;
  return {target};
}

MaterialLeaseFrameKey key(std::uint8_t payload, std::uint64_t target = 4,
                          std::uint64_t resources = 8) {
  return {{payload}, {7}, signature(), target, resources};
}
}

int main() {
  MaterialLeasePlanState state;
  state.setGenerations(4, 8);
  assert(state.requested(1, 20, signature()));
  assert(!state.granted(2, 100));
  assert(!state.granted(1, 0));
  assert(state.granted(1, 100));
  assert(state.initialRequested(21, key(1)));
  assert(!state.armed(20, 100));

  // Geometry may change before initial arm. A replacement active serial
  // invalidates the older serial while preserving the granted identity.
  assert(state.initialRequested(22, key(2)));
  assert(!state.armed(21, 100));
  assert(state.armed(22, 100));
  assert(!state.prepare(key(1)));
  // Layout may advance after the arm event but before the first buffer. Reissue
  // under the same token and require a fresh exact arm instead of restoring.
  assert(state.initialRequested(23, key(3)));
  assert(!state.prepare(key(2)));
  assert(state.armed(23, 100));
  auto first = state.prepare(key(3));
  assert(first && first->kind == PreparedMaterialLeaseFrame::Kind::First);
  assert(state.committed(*first));

  // Once active, bounded geometry/opacity/parameter payloads stream without a
  // new arm, but target and resource generations remain exact.
  auto moving = state.prepare(key(4));
  assert(moving && moving->kind == PreparedMaterialLeaseFrame::Kind::Streaming);
  assert(state.committed(*moving));
  assert(!state.prepare(key(4, 5, 8)));
  assert(!state.prepare(key(4, 4, 9)));
  auto differentTarget = key(4);
  differentTarget.signature = signature("bar.default.ram");
  assert(!state.prepare(differentTarget));

  // Revocation restores a token-zero descriptor with native pixels. A stale
  // release cannot clear the binding, and a token is never accepted twice.
  assert(state.revoked(100));
  assert(!state.armed(23, 100));
  assert(!state.granted(1, 102));
  auto restoreKey = key(5, 9, 10);
  restoreKey.signature = signature("bar.default.replacement");
  auto restore = state.prepareRestore(std::move(restoreKey));
  assert(restore && restore->kind == PreparedMaterialLeaseFrame::Kind::Restore);
  assert(state.restoreCommitted(*restore, 30));
  assert(!state.released(100, 29));
  assert(state.released(100, 30));
  assert(state.requested(2, 31, signature()));
  assert(!state.granted(2, 100));
  assert(state.granted(2, 101));

  // Disconnect resets the binding and permits a compositor's fresh token
  // namespace; stale callbacks from the destroyed object cannot reach it.
  state.resetBinding();
  state.setGenerations(4, 8);
  assert(state.requested(3, 40, signature()));
  assert(state.granted(3, 100));

  MaterialLeasePlanState pending;
  pending.setGenerations(1, 1);
  assert(pending.requested(8, 50, signature()));
  pending.invalidate();
  assert(pending.phase() == MaterialLeasePlanState::Phase::Idle);
  assert(!pending.granted(8, 77));
  return 0;
}
