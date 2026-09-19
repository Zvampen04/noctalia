#include "shell/settings/custom_effect_asset_service.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr std::string_view kValidEffect = R"(
// Bounded chromatic offset example.
vec4 noctalia_effect(vec4 source_pm, vec4 backdrop_pm, vec2 local_uv,
    vec2 local_px, vec2 size_px, vec4 p0, vec4 p1, vec4 p2, vec4 p3,
    vec4 p4, vec4 p5, vec4 p6, vec4 p7) {
  vec2 offset = vec2(p0.x, p0.y);
  return noctalia_sample_backdrop(offset) + source_pm * p1.x;
})";

std::filesystem::path uniqueDirectory() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() / ("noctalia-custom-effect-" + std::to_string(stamp));
}

void writePrivate(const std::filesystem::path& path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << content;
  output.close();
  std::filesystem::permissions(
      path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace
  );
}

} // namespace

int main() {
  const auto directory = uniqueDirectory();
  const auto dataHome = directory / "data";
  const auto source = directory / "source/effect.glsl";
  std::filesystem::remove_all(directory);
  writePrivate(source, kValidEffect);
  assert(::setenv("XDG_DATA_HOME", dataHome.c_str(), 1) == 0);

  assert(custom_effect_assets::validateSourceSubset(kValidEffect).empty());
  assert(!custom_effect_assets::validateSourceSubset("void main() {}").empty());
  assert(!custom_effect_assets::validateSourceSubset(
      "vec4 noctalia_effect(vec4 a){ return noctalia_effect(a); }").empty());
  assert(!custom_effect_assets::validateSourceSubset("/* unterminated").empty());

  const auto imported = custom_effect_assets::importFile(source, "user.chromatic", 24.0F);
  assert(imported);
  assert(imported.asset->stableId == "user.chromatic");
  assert(imported.asset->maxSampleRadiusPx == 24.0F);
  assert(imported.path == custom_effect_assets::importedPath(imported.asset->sha256Digest));
  assert(std::filesystem::is_regular_file(imported.path));
  const auto mode = std::filesystem::status(imported.path).permissions();
  assert((mode & (std::filesystem::perms::group_all | std::filesystem::perms::others_all))
      == std::filesystem::perms::none);

  const auto loaded = custom_effect_assets::loadImported(
      "user.chromatic", imported.asset->sha256Digest, 24.0F);
  assert(loaded);
  assert(loaded.asset->source == kValidEffect);
  assert(loaded.asset->sha256Digest == imported.asset->sha256Digest);

  const auto active = custom_effect_assets::resolveImported(
      "user.chromatic", imported.asset->sha256Digest, 24.0F);
  const auto sameActive = custom_effect_assets::resolveImported(
      "user.chromatic", imported.asset->sha256Digest, 24.0F);
  assert(active && sameActive);
  assert(active.asset == sameActive.asset);

  writePrivate(imported.path, std::string(kValidEffect) + "\n// changed");
  const auto tampered = custom_effect_assets::loadImported(
      "user.chromatic", imported.asset->sha256Digest, 24.0F);
  assert(!tampered);
  assert(tampered.error.contains("last known good"));

  const std::string replacementDigest(64, 'b');
  const auto lastGood = custom_effect_assets::resolveImported(
      "user.chromatic", replacementDigest, 24.0F);
  assert(lastGood && lastGood.usingLastGood);
  assert(lastGood.asset == active.asset);
  assert(!custom_effect_assets::diagnostic("user.chromatic").empty());

  const std::string missingDigest(64, 'a');
  const auto missing = custom_effect_assets::loadImported("user.missing", missingDigest, 0.0F);
  assert(!missing);
  assert(!missing.error.empty());
  assert(custom_effect_assets::importedPath("../escape").empty());

  std::filesystem::remove_all(directory);
  return 0;
}
