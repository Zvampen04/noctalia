#include "render/text/profile_font_resources.h"
#include "render/text/font_registry.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fontconfig/fontconfig.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace text {
namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;
constexpr std::size_t kFileLimit = 32 * 1024 * 1024;
constexpr std::size_t kTotalLimit = 128 * 1024 * 1024;
constexpr std::size_t kCountLimit = 128;
struct File {
  int fd = -1;
  explicit File(int value) : fd(value) {
    if (fd < 0) throw std::runtime_error("Cannot open imported font resource");
  }
  ~File() { if (fd >= 0) close(fd); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
};
struct Font {
  std::shared_ptr<File> file;
  std::size_t size = 0;
  std::vector<std::string> families;
  bool registered = false;
};
auto& retained() {
  static std::unordered_map<std::string, Font> fonts;
  return fonts;
}
bool digestName(const std::string& value) {
  return value.size() == 64 && std::ranges::all_of(value, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
fs::path resourceRoot() {
  const auto* data = std::getenv("XDG_DATA_HOME");
  fs::path root;
  if (data && *data) root = data;
  else {
    const auto* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("Cannot locate imported font directory");
    root = fs::path(home) / ".local/share";
  }
  if (!root.is_absolute() || root.lexically_normal() != root)
    throw std::runtime_error("Imported font data directory must be an absolute normalized path");
  return root / "fonts/noctalia";
}
std::shared_ptr<File> openResource(const fs::path& path, const fs::path& root) {
  if (!path.is_absolute() || path.lexically_normal() != path)
    throw std::runtime_error("Imported font path must be absolute and normalized");
  const auto relative = path.lexically_relative(root);
  std::vector<std::string> parts;
  for (const auto& item : relative) {
    const auto part = item.string();
    if (part.empty() || part == "." || part == "..")
      throw std::runtime_error("Imported font is outside its resource directory");
    parts.push_back(part);
  }
  if (parts.size() < 2 || parts.size() > 8 || !digestName(parts.front()))
    throw std::runtime_error("Imported font requires a content-addressed bundle directory");
  auto directory = std::make_shared<File>(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  fs::path walked = "/";
  for (auto it = path.begin(); it != path.end(); ++it) {
    if (*it == "/") continue;
    const bool last = std::next(it) == path.end();
    auto next = std::make_shared<File>(openat(directory->fd, it->c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (last ? 0 : O_DIRECTORY)));
    walked /= *it;
    struct stat info{};
    if (fstat(next->fd, &info) || (last ? !S_ISREG(info.st_mode) : !S_ISDIR(info.st_mode)))
      throw std::runtime_error("Imported font resource has an invalid file type");
    // Ancestors may belong to root. The managed resource tree belongs to this user.
    const auto below = walked.lexically_relative(root);
    if (walked == root || (!below.empty() && *below.begin() != "..")) {
      if (info.st_uid != getuid() || (info.st_mode & 0022))
        throw std::runtime_error("Imported font resources must be owned by this user and not writable by others");
    }
    directory = std::move(next);
  }
  return directory;
}
Font stage(const json& record, const fs::path& root) {
  if (!record.is_object() || record.size() != 4 || !record.contains("path")
      || !record.contains("family") || !record.contains("sha256") || !record.contains("size")
      || !record.at("path").is_string() || !record.at("family").is_string()
      || !record.at("sha256").is_string() || !record.at("size").is_number_unsigned())
    throw std::runtime_error("Imported font records require only path, family, sha256 and unsigned size");
  const auto path = record.at("path").get<std::string>();
  const auto hash = record.at("sha256").get<std::string>();
  const auto family = record.at("family").get<std::string>();
  if (path.size() > 4096 || path.find('\0') != std::string::npos || family.empty()
      || family.size() > 256 || family.find('\0') != std::string::npos || !digestName(hash)
      || !record.at("size").is_number_unsigned())
    throw std::runtime_error("Invalid imported font record");
  const auto size = record.at("size").get<std::uint64_t>();
  if (!size || size > kFileLimit) throw std::runtime_error("Imported font exceeds the file size limit");
  const auto source = openResource(path, root);
  struct stat info{};
  if (fstat(source->fd, &info) || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) != size)
    throw std::runtime_error("Imported font size differs from its manifest");
  std::vector<unsigned char> bytes(size);
  std::size_t offset = 0;
  while (offset < size) {
    const auto count = read(source->fd, bytes.data() + offset, size - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("Cannot read imported font bytes");
    offset += static_cast<std::size_t>(count);
  }
  std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
  crypto_hash_sha256(digest.data(), bytes.data(), bytes.size());
  std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
  sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
  if (hash != hex.data()) throw std::runtime_error("Imported font hash differs from its manifest");
  auto memory = std::make_shared<File>(memfd_create("noctalia-profile-font", MFD_CLOEXEC | MFD_ALLOW_SEALING));
  offset = 0;
  while (offset < size) {
    const auto count = write(memory->fd, bytes.data() + offset, size - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("Cannot retain imported font bytes");
    offset += static_cast<std::size_t>(count);
  }
  if (fcntl(memory->fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
    throw std::runtime_error("Cannot seal imported font bytes");
  const auto stable = "/proc/self/fd/" + std::to_string(memory->fd);
  const std::unique_ptr<FcFontSet, decltype(&FcFontSetDestroy)> fonts(FcFontSetCreate(), FcFontSetDestroy);
  const std::unique_ptr<FcStrSet, decltype(&FcStrSetDestroy)> dirs(FcStrSetCreate(), FcStrSetDestroy);
  if (!fonts || !dirs || !FcFileScan(fonts.get(), dirs.get(), nullptr, nullptr,
      reinterpret_cast<const FcChar8*>(stable.c_str()), FcTrue) || !fonts->nfont)
    throw std::runtime_error("Imported font cannot be decoded");
  Font result{memory, static_cast<std::size_t>(size), {}};
  for (int face = 0; face < fonts->nfont; ++face) {
    for (int index = 0;; ++index) {
      FcChar8* value = nullptr;
      if (FcPatternGetString(fonts->fonts[face], FC_FAMILY, index, &value) != FcResultMatch) break;
      result.families.emplace_back(reinterpret_cast<const char*>(value));
    }
  }
  if (!std::ranges::any_of(result.families, [&](const auto& candidate) {
        return FcStrCmpIgnoreCase(reinterpret_cast<const FcChar8*>(candidate.c_str()),
            reinterpret_cast<const FcChar8*>(family.c_str())) == 0;
      }))
    throw std::runtime_error("Imported font family differs from its manifest");
  return result;
}
} // namespace

nlohmann::json registerProfileFontResources(const nlohmann::json& records) {
  if (!records.is_array() || records.size() > 64)
    throw std::runtime_error("Expected at most 64 imported font records");
  // An empty guarded registration is a synchronization acknowledgement. It
  // must not depend on HOME/XDG resource discovery or touch Fontconfig.
  if (records.empty())
    return json{{"complete", true}, {"fonts", json::array()}, {"font_generation", fontConfigGeneration()}};
  const auto root = resourceRoot();
  std::unordered_map<std::string, Font> staged;
  std::size_t total = 0;
  std::uint64_t requestBytes = 0;
  for (const auto& [hash, font] : retained()) total += font.size;
  for (const auto& record : records) {
    if (!record.is_object() || !record.contains("size") || !record.at("size").is_number_unsigned()
        || record.at("size").get<std::uint64_t>() > kTotalLimit - requestBytes)
      throw std::runtime_error("Imported font request exceeds the byte limit");
    requestBytes += record.at("size").get<std::uint64_t>();
    // Validate even previously registered records: a stale/corrupt archive is not accepted.
    auto font = stage(record, root);
    const auto hash = record.at("sha256").get<std::string>();
    if (retained().contains(hash) || staged.contains(hash)) continue;
    total += font.size;
    if (total > kTotalLimit || retained().size() + staged.size() >= kCountLimit)
      throw std::runtime_error("Imported fonts exceed the process resource limit");
    staged.emplace(hash, std::move(font));
  }
  json result{{"complete", true}, {"fonts", json::array()}};
  for (const auto& record : records) {
    const auto hash = record.at("sha256").get<std::string>();
    if (!retained().contains(hash)) {
      auto node = staged.extract(hash);
      // Keep backing alive even if fontconfig partially accepts then reports an error.
      retained().insert(std::move(node));
    }
    auto& loaded = retained().at(hash);
    if (!loaded.registered) {
      const auto stable = "/proc/self/fd/" + std::to_string(loaded.file->fd);
      if (registerFontFile(stable).empty()) {
        result["complete"] = false;
        result["error"] = "Font registration failed; earlier returned resources remain available";
        break;
      }
      loaded.registered = true;
    }
    result["fonts"].push_back(record);
  }
  result["font_generation"] = fontConfigGeneration();
  return result;
}
} // namespace text
