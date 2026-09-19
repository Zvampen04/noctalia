#pragma once
#include "scene_descriptor.h"
#include <memory>

namespace noctalia::material {
// 8-byte Wayland header, serial and array length leave 4080 bytes.
inline constexpr std::size_t kMaxInlineSceneBytes = 4080;
inline constexpr std::size_t kSceneChunkBytes = 3072;
inline constexpr unsigned kSceneTransportVersion = 2;

// One bounded assembly per surface. No partial scene can be adopted.
class SceneAssembly {
public:
  enum class Result { Rejected, Pending, Complete };
  void reset() { bytes.clear(); total = 0; }
  void release() { std::vector<std::uint8_t>{}.swap(bytes); total = 0; }
  Result append(std::uint32_t serial, std::uint32_t size, std::uint32_t offset,
                std::span<const std::uint8_t> chunk) {
    if (!size || size > kMaxSceneBytes || chunk.empty() ||
        chunk.size() > kSceneChunkBytes || offset > size || chunk.size() > size - offset) {
      reset(); return Result::Rejected;
    }
    if (!offset) { reset(); sequence = serial; total = size; bytes.reserve(size); }
    if (serial != sequence || size != total || offset != bytes.size()) {
      reset(); return Result::Rejected;
    }
    bytes.insert(bytes.end(), chunk.begin(), chunk.end());
    return bytes.size() == total ? Result::Complete : Result::Pending;
  }
  std::span<const std::uint8_t> payload() const { return bytes; }
private:
  std::vector<std::uint8_t> bytes;
  std::uint32_t total = 0, sequence = 0;
};

// Protocol-independent double-buffered receiver state. Wayland resources remain
// compositor-owned; this class only validates bounded bytes and commit timing.
class SceneTransportState {
public:
  enum class Result { Rejected, Pending, Accepted };

  Result setInline(std::uint32_t serial, std::span<const std::uint8_t> payload) {
    upload.reset();
    if (payload.size() > kMaxInlineSceneBytes) return Result::Rejected;
    return accept(serial, payload);
  }

  Result appendChunk(std::uint32_t serial, std::uint32_t totalSize,
                     std::uint32_t offset, std::span<const std::uint8_t> payload) {
    switch (upload.append(serial, totalSize, offset, payload)) {
      case SceneAssembly::Result::Rejected: return Result::Rejected;
      case SceneAssembly::Result::Pending: return Result::Pending;
      case SceneAssembly::Result::Complete: {
        const auto result = accept(serial, upload.payload());
        upload.reset();
        return result;
      }
    }
    return Result::Rejected;
  }

  std::optional<std::uint32_t> commit() {
    upload.reset();
    if (!pending) return std::nullopt;
    current = std::move(pending);
    return pendingSerial;
  }

  void release() {
    upload.release();
    current.reset(); pending.reset(); pendingSerial = 0;
  }

  std::shared_ptr<const SceneDescriptor> scene() const { return current; }

private:
  Result accept(std::uint32_t serial, std::span<const std::uint8_t> payload) {
    auto decoded = decodeScene(payload);
    if (!decoded) return Result::Rejected;
    pending = std::make_shared<SceneDescriptor>(std::move(*decoded));
    pendingSerial = serial;
    return Result::Accepted;
  }

  SceneAssembly upload;
  std::shared_ptr<const SceneDescriptor> current, pending;
  std::uint32_t pendingSerial = 0;
};
}
