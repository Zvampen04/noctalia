#pragma once
#include "custom_effect_resource.h"
#include "material.h"
#include <algorithm>
#include <bit>
#include <string>
#include <string_view>
#include <vector>

namespace noctalia::material {
// v5/v6/v7 wire coordinates are logical wl_surface coordinates, before output scale,
// output transform and canvas projection. No handles, PIDs or device pixels.
inline constexpr std::uint32_t kLegacySceneVersion = 5, kSceneVersion = 6, kLeasedSceneVersion = 7;
inline constexpr std::size_t kMaxSceneBytes = 65536, kMaxScenePlanes = 64;
struct SceneRect {
  float x = 0, y = 0, width = 0, height = 0;
  bool operator==(const SceneRect&) const = default;
};
struct SceneRoundedClip {
  SceneRect bounds;
  float radius=0;
  float cornerPower=2;
  bool operator==(const SceneRoundedClip&) const = default;
};
struct SceneCustomEffect {
  CustomEffectTransportDigest transportDigest{};
  std::uint32_t abi = kCustomEffectAbi;
  float sampleRadius = 0;
  std::array<std::array<float,4>,8> parameters{};
  bool staged = true;
  bool operator==(const SceneCustomEffect&) const = default;
};
struct ScenePlane {
  std::uint32_t group = 0;
  std::string role = "surface", surface;
  float width = 0, height = 0;
  float cornerPower = 2;
  // x' = a*x+c*y+tx; y' = b*x+d*y+ty. Includes scene ancestors.
  std::array<float, 6> transform{1, 0, 0, 1, 0, 0};
  SceneRect clip;
  std::optional<SceneRoundedClip> paintClip;
  std::array<float, 4> radii{}, insets{}; // TL TR BR BL; left top right bottom
  std::uint32_t concaveCorners = 0; // TL bit0, TR bit1, BR bit2, BL bit3
  std::array<float, 4> tint{0, 0, 0, 1};
  float opacity = 1;
  Parameters parameters;
  std::optional<SceneCustomEffect> customEffect;
  bool operator==(const ScenePlane&) const = default;
};
struct SceneDescriptor {
  float width = 0, height = 0;
  std::vector<ScenePlane> planes;
  // Codec v7 only. Zero is a staged/native descriptor; a nonzero token names
  // one granted surface lease. Tokens never authorize older codec versions.
  std::uint32_t leaseToken = 0;
  bool operator==(const SceneDescriptor&) const = default;
};
inline bool sceneIdentifier(std::string_view id) {
  if (id.size() > 64) return false;
  for (const char c : id)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
  return true;
}
inline bool validateScene(const SceneDescriptor& scene) {
  const auto range = [](float n, float low, float high) { return std::isfinite(n) && n >= low && n <= high; };
  if (!range(scene.width, 1, 32768) || !range(scene.height, 1, 32768) || scene.planes.size() > kMaxScenePlanes) return false;
  for (std::size_t index = 0; index < scene.planes.size(); ++index) {
    const auto& p = scene.planes[index];
    if (!p.group || p.role.empty() || !sceneIdentifier(p.role) || !sceneIdentifier(p.surface)
        || !range(p.cornerPower,2,10) || !range(p.width, 0.01F, 32768) || !range(p.height, 0.01F, 32768)
        || p.concaveCorners > 15 || !range(p.opacity, 0, 1)) return false;
    for (std::size_t other = 0; other < index; ++other) if (p.group == scene.planes[other].group) return false;
    for (std::size_t i = 0; i < 6; ++i) if (!range(p.transform[i], i < 4 ? -64 : -65536, i < 4 ? 64 : 65536)) return false;
    const float determinant = p.transform[0] * p.transform[3] - p.transform[1] * p.transform[2];
    if (std::abs(determinant) < 0.0001F) return false;
    if (!range(p.clip.x, 0, scene.width) || !range(p.clip.y, 0, scene.height)
        || !range(p.clip.width, 0, scene.width - p.clip.x + 0.01F)
        || !range(p.clip.height, 0, scene.height - p.clip.y + 0.01F)) return false;
    if (p.paintClip) {
      const auto& mask=*p.paintClip;
      if (!range(mask.bounds.x,-65536,65536) || !range(mask.bounds.y,-65536,65536)
          || !range(mask.bounds.width,0,32768) || !range(mask.bounds.height,0,32768)
          || !range(mask.cornerPower,2,10) || !range(mask.radius,0,std::min(mask.bounds.width,mask.bounds.height)*0.5F)) return false;
    }
    if (!validScalarValue("optical_plane",p.parameters.optical.planeMode)) return false;
    if (!validScalarValue("lens_mapping",p.parameters.optical.lensMapping)) return false;
    for (float n : p.radii) if (!range(n, 0, 16384)) return false;
    for (float n : p.insets) if (!range(n, 0, 32768)) return false;
    for (float n : p.tint) if (!range(n, 0, 1)) return false;
    if (static_cast<unsigned>(p.parameters.primitive) > 3) return false;
    if (p.customEffect) {
      const auto& effect=*p.customEffect;
      if (effect.abi!=kCustomEffectAbi || !range(effect.sampleRadius,0,256)
          || effect.transportDigest==CustomEffectTransportDigest{}) return false;
      for(const auto& vector : effect.parameters) for(float n : vector)
        if (!range(n,-1024,1024)) return false;
    }
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) \
    if (!range(p.parameters.member, std::string_view(#member) == "lighting.direction.z" ? 0.001F : low, high)) return false;
#include "fields.def"
#undef MATERIAL_FIELD
  }
  return true;
}
namespace scene_wire {
struct Writer {
  std::vector<std::uint8_t> data;
  void integer(std::uint32_t n) { for (unsigned i = 0; i < 4; ++i) data.push_back(static_cast<std::uint8_t>(n >> (i * 8))); }
  void number(float n) { integer(std::bit_cast<std::uint32_t>(n)); }
  void raw(std::span<const std::uint8_t> bytes) { data.insert(data.end(),bytes.begin(),bytes.end()); }
  void string(std::string_view s) { integer(static_cast<std::uint32_t>(s.size())); data.insert(data.end(), s.begin(), s.end()); }
};
struct Reader {
  std::span<const std::uint8_t> data;
  std::size_t offset = 0;
  bool good = true;
  std::uint32_t integer() {
    if (offset + 4 > data.size()) { good = false; return 0; }
    std::uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(data[offset++]) << (i * 8);
    return n;
  }
  float number() { return std::bit_cast<float>(integer()); }
  template<std::size_t N> std::array<std::uint8_t,N> raw() {
    std::array<std::uint8_t,N> value{};
    if(offset+N>data.size()) {good=false;return value;}
    std::copy_n(data.begin()+offset,N,value.begin());offset+=N;return value;
  }
  std::string string() {
    const auto count = integer();
    if (!good || count > 64 || offset + count > data.size()) { good = false; return {}; }
    std::string value(reinterpret_cast<const char*>(data.data() + offset), count);
    offset += count;
    return value;
  }
};
} // namespace scene_wire
inline std::optional<std::uint32_t> sceneWireVersion(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 8 || bytes.size() > kMaxSceneBytes) return std::nullopt;
  scene_wire::Reader r{bytes};
  if (r.integer() != 0x4d53434e) return std::nullopt;
  const auto version = r.integer();
  if (!r.good || (version != kLegacySceneVersion && version != kSceneVersion
      && version != kLeasedSceneVersion)) return std::nullopt;
  return version;
}
// Automatic mode preserves v5 for scenes without custom effects. A v3 sender
// explicitly requests v6 for staged/active effect handshakes.
inline std::vector<std::uint8_t> encodeScene(const SceneDescriptor& scene,std::uint32_t version=0) {
  if(!version) {
    version=kLegacySceneVersion;
    for(const auto& plane:scene.planes) if(plane.customEffect){version=kSceneVersion;break;}
  }
  if (!validateScene(scene) || (version != kLegacySceneVersion && version != kSceneVersion
      && version != kLeasedSceneVersion)) return {};
  if (version == kLegacySceneVersion)
    for (const auto& plane : scene.planes) if (plane.customEffect) return {};
  if (version < kLeasedSceneVersion && scene.leaseToken != 0) return {};
  scene_wire::Writer w;
  w.integer(0x4d53434e); // NCSM in little-endian byte order
  w.integer(version);
  w.number(scene.width); w.number(scene.height);
  w.integer(static_cast<std::uint32_t>(scene.planes.size()));
  if (version >= kLeasedSceneVersion) w.integer(scene.leaseToken);
  for (const auto& p : scene.planes) {
    w.integer(p.group); w.string(p.role); w.string(p.surface);
    w.number(p.width); w.number(p.height);
    for (float n : p.transform) w.number(n);
    w.number(p.clip.x); w.number(p.clip.y); w.number(p.clip.width); w.number(p.clip.height);
    w.integer(p.paintClip.has_value());
    if (p.paintClip) {
      const auto& mask=*p.paintClip;
      w.number(mask.bounds.x);w.number(mask.bounds.y);w.number(mask.bounds.width);w.number(mask.bounds.height);w.number(mask.radius); w.number(mask.cornerPower);
    }
    w.number(p.cornerPower);
    for (float n : p.radii) w.number(n);
    for (float n : p.insets) w.number(n);
    w.integer(p.concaveCorners);
    for (float n : p.tint) w.number(n);
    w.number(p.opacity); w.integer(static_cast<std::uint32_t>(p.parameters.primitive));
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) w.number(p.parameters.member);
#include "fields.def"
#undef MATERIAL_FIELD
    w.integer(p.parameters.illustration.seed);
    if(version>=kSceneVersion) {
      w.integer(p.customEffect.has_value());
      if(p.customEffect) {
        const auto& effect=*p.customEffect;w.raw(effect.transportDigest);w.integer(effect.abi);w.number(effect.sampleRadius);
        for(const auto& vector : effect.parameters) for(float n : vector) w.number(n);
        w.integer(effect.staged);
      }
    }
  }
  return w.data.size() <= kMaxSceneBytes ? std::move(w.data) : std::vector<std::uint8_t>{};
}
// Reject atomically; callers retain their last valid committed scene.
inline std::optional<SceneDescriptor> decodeScene(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > kMaxSceneBytes) return std::nullopt;
  scene_wire::Reader r{bytes};
  if (r.integer() != 0x4d53434e) return std::nullopt;
  const auto version=r.integer();
  if (version != kLegacySceneVersion && version != kSceneVersion && version != kLeasedSceneVersion)
    return std::nullopt;
  SceneDescriptor scene;
  scene.width = r.number(); scene.height = r.number();
  const auto count = r.integer();
  if (version >= kLeasedSceneVersion) scene.leaseToken = r.integer();
  if (!r.good || count > kMaxScenePlanes) return std::nullopt;
  for (std::uint32_t i = 0; i < count && r.good; ++i) {
    ScenePlane p;
    p.group = r.integer(); p.role = r.string(); p.surface = r.string();
    p.width = r.number(); p.height = r.number();
    for (float& n : p.transform) n = r.number();
    p.clip = {r.number(), r.number(), r.number(), r.number()};
    const auto hasMask=r.integer();
    if (hasMask>1) return std::nullopt;
    if (hasMask) p.paintClip=SceneRoundedClip{{r.number(),r.number(),r.number(),r.number()},r.number(),r.number()};
    p.cornerPower = r.number();
    for (float& n : p.radii) n = r.number();
    for (float& n : p.insets) n = r.number();
    p.concaveCorners = r.integer();
    for (float& n : p.tint) n = r.number();
    p.opacity = r.number();
    const auto primitive = r.integer();
    if (primitive > 3) return std::nullopt;
    p.parameters.primitive = static_cast<Primitive>(primitive);
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) p.parameters.member = r.number();
#include "fields.def"
#undef MATERIAL_FIELD
    p.parameters.illustration.seed = r.integer();
    if(version>=kSceneVersion) {
      const auto hasEffect=r.integer();if(hasEffect>1) return std::nullopt;
      if(hasEffect) {
        SceneCustomEffect effect;effect.transportDigest=r.raw<kCustomEffectTransportDigestBytes>();effect.abi=r.integer();effect.sampleRadius=r.number();
        for(auto& vector : effect.parameters) for(float& n : vector) n=r.number();
        const auto staged=r.integer();if(staged>1) return std::nullopt;effect.staged=staged;p.customEffect=effect;
      }
    }
    scene.planes.push_back(std::move(p));
  }
  if (!r.good || r.offset != bytes.size() || !validateScene(scene)) return std::nullopt;
  return scene;
}
} // namespace noctalia::material
