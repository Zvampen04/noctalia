#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

inline constexpr std::uint32_t kCustomEffectAbiVersion = 1;
inline constexpr std::size_t kCustomEffectMaxSourceBytes = 32 * 1024;
inline constexpr float kCustomEffectMaxSampleRadiusPx = 256.0F;

struct CustomEffectAsset {
  std::string stableId;
  std::string sha256Digest;
  std::string source;
  std::uint32_t abi = kCustomEffectAbiVersion;
  float maxSampleRadiusPx = 0.0F;
  bool operator==(const CustomEffectAsset&) const = default;
};

// ABI 1 source defines:
// vec4 noctalia_effect(vec4 source_pm, vec4 backdrop_pm, vec2 local_uv,
//   vec2 local_px, vec2 size_px, vec4 p0, ... vec4 p7);
// It may call noctalia_sample_source/backdrop(offset_local_px). The return is
// straight RGB plus an opacity multiplier; the host applies carrier opacity,
// rounded coverage and premultiplication.

struct CustomEffectBinding {
  std::shared_ptr<const CustomEffectAsset> asset;
  std::array<std::array<float, 4>, 8> parameters{};
  bool operator==(const CustomEffectBinding&) const = default;
};

struct CustomEffectCompileStatus {
  std::string stableId;
  std::string activeDigest;
  std::string requestedDigest;
  std::string log;
  bool active = false;
  bool operator==(const CustomEffectCompileStatus&) const = default;
};

[[nodiscard]] bool validCustomEffectAsset(const CustomEffectAsset& asset, std::string* error = nullptr);
[[nodiscard]] bool stripCustomEffectComments(std::string_view source,std::string& stripped,std::string* error=nullptr);
