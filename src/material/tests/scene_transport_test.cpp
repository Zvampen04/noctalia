#include "scene_transport.h"
#include <cstdlib>
#include <iostream>

using namespace noctalia::material;
#define CHECK(x) do { if (!(x)) { std::cerr << "failed: " #x << '\n'; std::exit(1); } } while (false)

namespace {
SceneDescriptor scene(float width, std::size_t planeCount = 0) {
  SceneDescriptor result{width, 600, {}};
  for (std::size_t i = 0; i < planeCount; ++i) {
    ScenePlane plane;
    plane.group = static_cast<std::uint32_t>(i + 1);
    plane.width = 200; plane.height = 50;
    plane.clip = {0, 0, width, 600};
    plane.surface = "panel";
    result.planes.push_back(plane);
  }
  return result;
}

std::vector<std::uint8_t> exactInlineWire() {
  for (std::size_t count = 1; count <= kMaxScenePlanes; ++count) {
    auto candidate = scene(800, count);
    auto wire = encodeScene(candidate);
    if (wire.size() > kMaxInlineSceneBytes) continue;
    auto remaining = kMaxInlineSceneBytes - wire.size();
    if (remaining > count * 59) continue;
    for (auto& plane : candidate.planes) {
      const auto added = std::min<std::size_t>(remaining, 59);
      plane.surface.append(added, 'a');
      remaining -= added;
    }
    wire = encodeScene(candidate);
    if (wire.size() == kMaxInlineSceneBytes) return wire;
  }
  return {};
}

SceneTransportState::Result appendAll(SceneTransportState& state, std::uint32_t serial,
                                      const std::vector<std::uint8_t>& wire) {
  SceneTransportState::Result result = SceneTransportState::Result::Pending;
  for (std::size_t offset = 0; offset < wire.size(); offset += kSceneChunkBytes) {
    const auto length = std::min(kSceneChunkBytes, wire.size() - offset);
    result = state.appendChunk(serial, wire.size(), offset,
                               std::span(wire.data() + offset, length));
  }
  return result;
}
}

int main() {
  CHECK(kMaxInlineSceneBytes == 4080);
  CHECK(kSceneChunkBytes == 3072);
  CHECK(kSceneTransportVersion == 2);

  // Exercise assembly limits independently of descriptor validation.
  SceneAssembly assembly;
  std::vector<std::uint8_t> chunk(kSceneChunkBytes + 1, 1);
  CHECK(assembly.append(1, chunk.size(), 0, chunk) == SceneAssembly::Result::Rejected);
  CHECK(assembly.append(1, 1, 0, {}) == SceneAssembly::Result::Rejected);
  CHECK(assembly.append(1, 0, 0, std::span(chunk.data(), 1)) == SceneAssembly::Result::Rejected);
  CHECK(assembly.append(1, kMaxSceneBytes + 1, 0, std::span(chunk.data(), 1)) == SceneAssembly::Result::Rejected);
  CHECK(assembly.append(1, 4, 5, std::span(chunk.data(), 1)) == SceneAssembly::Result::Rejected);
  CHECK(assembly.append(1, 4, 3, std::span(chunk.data(), 2)) == SceneAssembly::Result::Rejected);
  std::vector<std::uint8_t> maximum(kMaxSceneBytes, 1);
  SceneAssembly::Result maximumResult = SceneAssembly::Result::Pending;
  for (std::size_t offset = 0; offset < maximum.size(); offset += kSceneChunkBytes) {
    const auto length = std::min(kSceneChunkBytes, maximum.size() - offset);
    maximumResult = assembly.append(2, maximum.size(), offset, std::span(maximum.data() + offset, length));
  }
  CHECK(maximumResult == SceneAssembly::Result::Complete && assembly.payload().size() == kMaxSceneBytes);

  const auto first = scene(800);
  const auto second = scene(900);
  const auto firstWire = encodeScene(first);
  const auto secondWire = encodeScene(second);
  CHECK(!firstWire.empty() && firstWire.size() <= kMaxInlineSceneBytes);

  SceneTransportState state;
  const auto inlineLimit = exactInlineWire();
  CHECK(inlineLimit.size() == kMaxInlineSceneBytes);
  CHECK(state.setInline(0, inlineLimit) == SceneTransportState::Result::Accepted);
  CHECK(state.commit() == 0);
  CHECK(!state.commit());
  state.release();
  CHECK(state.setInline(1, firstWire) == SceneTransportState::Result::Accepted);
  CHECK(!state.scene());
  CHECK(state.commit() == 1);
  CHECK(*state.scene() == first);

  // A rejected inline update neither replaces current state nor an earlier
  // valid pending update. The legacy size boundary is enforced before decode.
  CHECK(state.setInline(2, secondWire) == SceneTransportState::Result::Accepted);
  std::vector<std::uint8_t> oversized(kMaxInlineSceneBytes + 1, 0);
  CHECK(state.setInline(3, oversized) == SceneTransportState::Result::Rejected);
  CHECK(state.commit() == 2);
  CHECK(*state.scene() == second);

  const auto large = scene(1024, kMaxScenePlanes);
  const auto largeWire = encodeScene(large);
  CHECK(largeWire.size() > kMaxInlineSceneBytes && largeWire.size() <= kMaxSceneBytes);
  CHECK(appendAll(state, 4, largeWire) == SceneTransportState::Result::Accepted);
  CHECK(*state.scene() == second);
  CHECK(state.commit() == 4);
  CHECK(*state.scene() == large);

  // An incomplete upload is dropped at commit. A previously validated pending
  // descriptor still commits atomically.
  CHECK(state.setInline(5, firstWire) == SceneTransportState::Result::Accepted);
  CHECK(state.appendChunk(6, largeWire.size(), 0,
                          std::span(largeWire.data(), kSceneChunkBytes)) == SceneTransportState::Result::Pending);
  CHECK(state.commit() == 5);
  CHECK(*state.scene() == first);
  CHECK(state.appendChunk(6, largeWire.size(), kSceneChunkBytes,
                          std::span(largeWire.data() + kSceneChunkBytes, 1)) == SceneTransportState::Result::Rejected);

  CHECK(state.appendChunk(60, largeWire.size(), 0,
                          std::span(largeWire.data(), kSceneChunkBytes)) == SceneTransportState::Result::Pending);
  CHECK(state.setInline(61, secondWire) == SceneTransportState::Result::Accepted);
  CHECK(state.appendChunk(60, largeWire.size(), kSceneChunkBytes,
                          std::span(largeWire.data() + kSceneChunkBytes, 1)) == SceneTransportState::Result::Rejected);
  CHECK(state.commit() == 61 && *state.scene() == second);

  // Offset zero replaces an upload. Old serials, totals and non-contiguous
  // offsets reject and discard only the upload, preserving valid scene state.
  CHECK(state.appendChunk(7, largeWire.size(), 0,
                          std::span(largeWire.data(), kSceneChunkBytes)) == SceneTransportState::Result::Pending);
  CHECK(state.appendChunk(8, largeWire.size(), 0,
                          std::span(largeWire.data(), kSceneChunkBytes)) == SceneTransportState::Result::Pending);
  CHECK(state.appendChunk(7, largeWire.size(), kSceneChunkBytes,
                          std::span(largeWire.data() + kSceneChunkBytes, 1)) == SceneTransportState::Result::Rejected);
  CHECK(*state.scene() == second);
  CHECK(state.appendChunk(9, largeWire.size(), 0,
                          std::span(largeWire.data(), kSceneChunkBytes)) == SceneTransportState::Result::Pending);
  CHECK(state.appendChunk(9, largeWire.size() - 1, kSceneChunkBytes,
                          std::span(largeWire.data() + kSceneChunkBytes, 1)) == SceneTransportState::Result::Rejected);
  CHECK(state.appendChunk(10, largeWire.size(), 0,
                          std::span(largeWire.data(), kSceneChunkBytes)) == SceneTransportState::Result::Pending);
  CHECK(state.appendChunk(10, largeWire.size(), kSceneChunkBytes + 1,
                          std::span(largeWire.data() + kSceneChunkBytes, 1)) == SceneTransportState::Result::Rejected);

  // A syntactically complete but invalid descriptor cannot replace a valid
  // pending descriptor, and release removes committed state.
  CHECK(state.setInline(11, secondWire) == SceneTransportState::Result::Accepted);
  auto invalid = largeWire;
  invalid[0] ^= 0xff;
  CHECK(appendAll(state, 12, invalid) == SceneTransportState::Result::Rejected);
  CHECK(state.commit() == 11);
  CHECK(*state.scene() == second);
  state.release();
  CHECK(!state.scene());
  CHECK(!state.commit());

  std::cout << "scene transport boundaries, replacement and atomic commit passed\n";
}
