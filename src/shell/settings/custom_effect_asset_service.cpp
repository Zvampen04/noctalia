#include "shell/settings/custom_effect_asset_service.h"

#include "config/atomic_file.h"
#include "util/file_utils.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sodium.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace custom_effect_assets {
namespace {

struct CacheEntry {
  std::weak_ptr<const CustomEffectAsset> active;
  std::string activeDigest;
  std::string requestedDigest;
  std::string error;
};

std::mutex g_cacheMutex;
std::map<std::string, CacheEntry, std::less<>> g_cache;

std::filesystem::path assetRoot() {
  std::filesystem::path root;
  if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && *data != '\0')
    root = std::filesystem::path(data) / "noctalia/custom-effects";
  else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    root = std::filesystem::path(home) / ".local/share/noctalia/custom-effects";
  if (!root.is_absolute() || root.lexically_normal() != root) return {};
  return root;
}

bool validDigest(std::string_view value) {
  return value.size() == crypto_hash_sha256_BYTES * 2
      && std::ranges::all_of(value, [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

std::string digest(std::string_view source) {
  std::array<unsigned char, crypto_hash_sha256_BYTES> bytes{};
  crypto_hash_sha256(bytes.data(), reinterpret_cast<const unsigned char*>(source.data()), source.size());
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  sodium_bin2hex(hex.data(), hex.size(), bytes.data(), bytes.size());
  return hex.data();
}

StoredAsset stage(
    std::string source, std::filesystem::path path, std::string stableId,
    std::string expectedDigest, float radius
) {
  if (const auto error = validateSourceSubset(source); !error.empty()) return {.error = error};
  const std::string actualDigest = digest(source);
  if (!expectedDigest.empty() && expectedDigest != actualDigest)
    return {.error = "Imported custom effect changed on disk; the last known good effect remains active"};
  auto asset = std::make_shared<CustomEffectAsset>(CustomEffectAsset{
      .stableId = std::move(stableId), .sha256Digest = actualDigest, .source = std::move(source),
      .abi = kCustomEffectAbiVersion, .maxSampleRadiusPx = radius,
  });
  std::string error;
  if (!validCustomEffectAsset(*asset, &error)) return {.error = std::move(error)};
  return {.asset = std::move(asset), .path = std::move(path)};
}

StoredAsset readOwned(
    const std::filesystem::path& path, std::string stableId, std::string expectedDigest, float radius
) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return {.error = "Cannot open the custom effect file"};
  struct stat info{};
  const bool invalid = ::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::getuid()
      || (info.st_mode & 0022) != 0 || info.st_size <= 0
      || static_cast<std::uint64_t>(info.st_size) > kCustomEffectMaxSourceBytes;
  if (invalid) {
    ::close(fd);
    return {.error = "Custom effect files must be user-owned regular files, not writable by other users, and no larger than 32 KiB"};
  }
  std::string source(static_cast<std::size_t>(info.st_size), '\0');
  std::size_t offset = 0;
  while (offset < source.size()) {
    const auto count = ::read(fd, source.data() + offset, source.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ::close(fd);
      return {.error = "Cannot read the complete custom effect file"};
    }
    offset += static_cast<std::size_t>(count);
  }
  ::close(fd);
  return stage(std::move(source), path, std::move(stableId), std::move(expectedDigest), radius);
}

StoredAsset persist(StoredAsset loaded) {
  if (!loaded) return loaded;
  const auto root = assetRoot();
  if (root.empty()) return {.error = "Cannot locate the custom effect data directory"};
  const auto directory = root / loaded.asset->sha256Digest;
  std::error_code ec;
  if (!FileUtils::createPrivateDirectories(directory, ec))
    return {.error = "Cannot create the private custom effect directory"};
  const auto destination = directory / "effect.glsl";
  if (!writeTextFileAtomic(destination, loaded.asset->source, FileUtils::privateFileMode()))
    return {.error = "Cannot store the imported custom effect"};
  loaded.path = destination;
  return loaded;
}

} // namespace

std::string validateSourceSubset(std::string_view source) {
  CustomEffectAsset candidate{
      .stableId = "validation.asset",
      .sha256Digest = std::string(64, '0'),
      .source = std::string(source),
  };
  std::string error;
  (void)validCustomEffectAsset(candidate, &error);
  return error;
}

StoredAsset importFile(const std::filesystem::path& sourcePath, std::string_view stableId, float radius) {
  return persist(readOwned(sourcePath, std::string(stableId), {}, radius));
}

StoredAsset installSealed(
    std::string_view source, std::string_view stableId, std::string_view expectedDigest, float radius
) {
  return persist(stage(
      std::string(source), {}, std::string(stableId), std::string(expectedDigest), radius));
}

std::filesystem::path importedPath(std::string_view expectedDigest) {
  if (!validDigest(expectedDigest)) return {};
  const auto root = assetRoot();
  if (root.empty()) return {};
  return root / std::string(expectedDigest) / "effect.glsl";
}

StoredAsset loadImported(
    std::string_view stableId, std::string_view expectedDigest, float radius
) {
  const auto storedPath = importedPath(expectedDigest);
  if (storedPath.empty()) return {.error = "The imported custom effect identity is invalid"};
  return readOwned(storedPath, std::string(stableId), std::string(expectedDigest), radius);
}

ResolvedAsset resolveImported(
    std::string_view stableId, std::string_view expectedDigest, float radius
) {
  std::shared_ptr<const CustomEffectAsset> previous;
  {
    const std::scoped_lock lock(g_cacheMutex);
    if (const auto found = g_cache.find(stableId); found != g_cache.end()) {
      previous = found->second.active.lock();
      if (previous && found->second.activeDigest == expectedDigest
          && previous->maxSampleRadiusPx == radius)
        return {.asset = std::move(previous), .error = found->second.error};
    }
  }

  auto loaded = loadImported(stableId, expectedDigest, radius);
  const std::scoped_lock lock(g_cacheMutex);
  auto& entry = g_cache[std::string(stableId)];
  entry.requestedDigest = expectedDigest;
  if (loaded) {
    entry.active = loaded.asset;
    entry.activeDigest = loaded.asset->sha256Digest;
    entry.error.clear();
    return {.asset = std::move(loaded.asset)};
  }
  entry.error = loaded.error;
  if (previous) {
    entry.active = previous;
    return {.asset = std::move(previous), .error = entry.error, .usingLastGood = true};
  }
  entry.active.reset();
  entry.activeDigest.clear();
  return {.error = entry.error};
}

std::string diagnostic(std::string_view stableId) {
  const std::scoped_lock lock(g_cacheMutex);
  if (const auto found = g_cache.find(stableId); found != g_cache.end()) return found->second.error;
  return {};
}

void invalidate(std::string_view stableId) {
  const std::scoped_lock lock(g_cacheMutex);
  g_cache.erase(std::string(stableId));
}

} // namespace custom_effect_assets
