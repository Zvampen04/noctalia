#pragma once

#include "render/custom_effect/custom_effect_types.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace custom_effect_assets {

struct StoredAsset {
  std::shared_ptr<const CustomEffectAsset> asset;
  std::filesystem::path path;
  std::string error;
  [[nodiscard]] explicit operator bool() const noexcept { return asset != nullptr; }
};

struct ResolvedAsset {
  std::shared_ptr<const CustomEffectAsset> asset;
  std::string error;
  bool usingLastGood = false;
  [[nodiscard]] explicit operator bool() const noexcept { return asset != nullptr; }
};

[[nodiscard]] std::string validateSourceSubset(std::string_view source);
[[nodiscard]] StoredAsset importFile(
    const std::filesystem::path& sourcePath, std::string_view stableId, float maxSampleRadiusPx
);
[[nodiscard]] StoredAsset installSealed(
    std::string_view source, std::string_view stableId, std::string_view expectedDigest,
    float maxSampleRadiusPx
);
[[nodiscard]] std::filesystem::path importedPath(std::string_view sha256Digest);
[[nodiscard]] StoredAsset loadImported(
    std::string_view stableId, std::string_view expectedDigest, float maxSampleRadiusPx
);
[[nodiscard]] ResolvedAsset resolveImported(
    std::string_view stableId, std::string_view expectedDigest, float maxSampleRadiusPx
);
[[nodiscard]] std::string diagnostic(std::string_view stableId);
void invalidate(std::string_view stableId);

} // namespace custom_effect_assets
