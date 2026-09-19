#include "config/config_service.h"
#include "config/config_export.h"
#include "config/profile_scope.h"
#include "config/profile_values.h"
#include "render/text/profile_font_resources.h"
#include "render/text/font_registry.h"
#include "shell/settings/custom_effect_asset_service.h"
#include "ui/material_overrides.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fcntl.h>
#include <map>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
using json = nlohmann::json;
json asJson(const toml::table& table) {
  std::ostringstream out;
  out << toml::json_formatter{table};
  return json::parse(out.str());
}
const std::string& sessionId() {
  static const auto id = std::to_string(getpid()) + "-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  return id;
}
json readRequest(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) throw std::runtime_error("Cannot open profile request");
  struct Close { int fd; ~Close() { close(fd); } } closeFile{fd};
  struct stat info{};
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid()
      || (info.st_mode & 0077) || info.st_size < 0 || info.st_size > 2 * 1024 * 1024)
    throw std::runtime_error("Profile request must be a private file smaller than 2 MiB");
  std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = read(fd, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) throw std::runtime_error("Cannot read profile request");
    offset += static_cast<std::size_t>(count);
  }
  return json::parse(bytes);
}

json registerCustomEffectResources(const json& records) {
  if (!records.is_array() || records.size() > 32)
    throw std::runtime_error("Expected at most 32 imported custom effect records");
  std::size_t total = 0;
  struct EffectRecord {
    std::string stableId;
    std::string digest;
    std::string source;
    float radius = 0.0F;
  };
  std::vector<EffectRecord> staged;
  staged.reserve(records.size());
  json installed = json::array();
  for (const auto& record : records) {
    if (!record.is_object() || record.size() != 6
        || !record.contains("stable_id") || !record.contains("sha256")
        || !record.contains("size") || !record.contains("abi")
        || !record.contains("max_sample_radius") || !record.contains("source")
        || !record.at("stable_id").is_string() || !record.at("sha256").is_string()
        || !record.at("size").is_number_unsigned() || !record.at("abi").is_number_unsigned()
        || !record.at("max_sample_radius").is_number()
        || !record.at("source").is_string())
      throw std::runtime_error("Invalid imported custom effect resource manifest");
    const auto source = record.at("source").get<std::string>();
    const auto size = record.at("size").get<std::size_t>();
    if (size != source.size() || size == 0 || size > kCustomEffectMaxSourceBytes
        || total > 512 * 1024 - size)
      throw std::runtime_error("Imported custom effect size differs from its bounded manifest");
    if (record.at("abi").get<std::uint32_t>() != kCustomEffectAbiVersion)
      throw std::runtime_error("Unsupported imported custom effect ABI");
    const double radius = record.at("max_sample_radius").get<double>();
    if (!std::isfinite(radius) || radius < 0.0 || radius > kCustomEffectMaxSampleRadiusPx)
      throw std::runtime_error("Imported custom effect sample radius is outside 0..256 pixels");
    staged.push_back(EffectRecord{
        .stableId = record.at("stable_id").get<std::string>(),
        .digest = record.at("sha256").get<std::string>(),
        .source = source,
        .radius = static_cast<float>(radius),
    });
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash{};
    crypto_hash_sha256(hash.data(), reinterpret_cast<const unsigned char*>(source.data()), source.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), hash.data(), hash.size());
    if (staged.back().digest != hex.data())
      throw std::runtime_error("Imported custom effect hash differs from its manifest");
    const CustomEffectAsset descriptor{
        .stableId = staged.back().stableId, .sha256Digest = staged.back().digest,
        .source = staged.back().source, .abi = kCustomEffectAbiVersion,
        .maxSampleRadiusPx = staged.back().radius,
    };
    std::string descriptorError;
    const auto subsetError = custom_effect_assets::validateSourceSubset(source);
    if (!validCustomEffectAsset(descriptor, &descriptorError) || !subsetError.empty())
      throw std::runtime_error(descriptorError.empty() ? subsetError : descriptorError);
    total += size;
  }
  for (const auto& record : staged) {
    auto result = custom_effect_assets::installSealed(
        record.source, record.stableId, record.digest, record.radius);
    if (!result) throw std::runtime_error(result.error);
    installed.push_back(json{{"stable_id", result.asset->stableId},
                             {"sha256", result.asset->sha256Digest}, {"size", record.source.size()}});
  }
  return json{{"complete", true}, {"effects", std::move(installed)}};
}

json exportCustomEffectResources(const Style::MaterialOverrides& overrides) {
  struct Reference { std::string digest; float radius = 0.0F; };
  std::map<std::string, Reference, std::less<>> references;
  const auto collect = [&references](const Style::MaterialOverrideMap& scope) {
    for (const auto& [target, patch] : scope) {
      (void)target;
      if (!patch.customBackground && !patch.customBackgroundDigest) continue;
      if (!patch.customBackground || !patch.customBackgroundDigest)
        throw std::runtime_error("Custom effect config has an incomplete asset reference");
      const Reference reference{*patch.customBackgroundDigest, patch.customSampleRadiusPx.value_or(0.0F)};
      const auto [found, inserted] = references.emplace(*patch.customBackground, reference);
      if (!inserted && found->second.digest != reference.digest)
        throw std::runtime_error("One custom effect identity refers to conflicting assets");
      if (!inserted) found->second.radius = std::max(found->second.radius, reference.radius);
    }
  };
  collect(overrides.roles);
  collect(overrides.families);
  collect(overrides.surfaces);
  json result = json::array();
  std::size_t total = 0;
  for (const auto& [stableId, reference] : references) {
    const auto stored = custom_effect_assets::loadImported(
        stableId, reference.digest, reference.radius);
    if (!stored) throw std::runtime_error(stored.error);
    if (total > 512 * 1024 - stored.asset->source.size())
      throw std::runtime_error("Custom effect profile resources exceed 512 KiB");
    total += stored.asset->source.size();
    result.push_back(json{
        {"stable_id", stableId}, {"sha256", reference.digest},
        {"size", stored.asset->source.size()}, {"abi", stored.asset->abi},
        {"max_sample_radius", stored.asset->maxSampleRadiusPx}, {"source", stored.asset->source},
    });
  }
  return result;
}
}

void ConfigService::beginProfilePreview() {
  if (m_profilePreview) return;
  m_profileBaseline = m_persistedOverridesTable;
  const auto baseline = configForOverrides(m_profileBaseline);
  m_profileEffectiveBaseline = config_export::serialize(baseline ? *baseline : m_config);
  m_profileConflict = false;
  m_profilePreview = true;
  ++m_profileGeneration;
}

bool ConfigService::profilePreviewDirty() const {
  return m_profilePreview && (m_profileExternalDirty || noctalia::profile::subset(
      config_export::serialize(m_config)) != noctalia::profile::subset(m_profileEffectiveBaseline));
}

bool ConfigService::commitProfilePreview() {
  if (!m_profilePreview) return true;
  const auto preparation = profilePreparation();
  if (preparation.pending || !preparation.error.empty()) {
    m_lastMutationError = preparation.pending ? "Application appearance is still being applied" : preparation.error;
    return false;
  }
  // Read pending external writes before committing, even when the file watcher
  // has not delivered its event yet. A conflict retains the live draft.
  loadOverridesFromFile();
  if (m_profileConflict || !m_overridesParseError.empty()) {
    m_lastMutationError = "Appearance changed outside this preview; cancel and reload before saving";
    return false;
  }
  const auto committed = configForOverrides(m_profileBaseline);
  if (!committed || noctalia::profile::subset(config_export::serialize(*committed))
      != noctalia::profile::subset(m_profileEffectiveBaseline)) {
    m_profileConflict = true;
    m_lastMutationError = "Appearance configuration changed outside this preview";
    return false;
  }
  const auto beforeReceipt = m_overridesTable;
  if (!m_overridesTable["plugin_settings"].is_table())
    m_overridesTable.insert_or_assign("plugin_settings", toml::table{});
  m_overridesTable["plugin_settings"].as_table()->insert_or_assign("__profile_commit", toml::table{
      {"session", sessionId()}, {"generation", static_cast<std::int64_t>(m_profileGeneration)}});
  // This receipt is part of the same atomic file replacement as the appearance.
  // Identical native values still need proof of a WM-only transaction commit.
  m_profileCommitting = true;
  const bool written = writeOverridesToFile();
  m_profileCommitting = false;
  if (!written) {
    m_overridesTable = beforeReceipt;
    return false;
  }
  m_profilePreview = false;
  m_profileExternalDirty = false;
  m_profileBaseline.clear();
  m_profileEffectiveBaseline.clear();
  loadAll();
  fireReloadCallbacks();
  return true;
}

void ConfigService::cancelProfilePreview() {
  if (!m_profilePreview) return;
  loadOverridesFromFile();
  noctalia::profile::replace(m_overridesTable, m_persistedOverridesTable);
  m_profilePreview = false;
  m_profileExternalDirty = false;
  m_profileConflict = false;
  m_profileBaseline.clear();
  m_profileEffectiveBaseline.clear();
  ++m_profileRevision;
  loadAll();
  fireReloadCallbacks();
}

std::string ConfigService::profileRequest(const std::string& request) {
  try {
    const auto split = request.find(' ');
    const auto action = request.substr(0, split);
    const bool readOnly = action == "status" || action == "snapshot" || action == "committed"
        || action == "resource-export";
    json payload;
    json resources;
    if (!readOnly) {
      if (split == std::string::npos) throw std::runtime_error("Missing guarded profile request file");
      payload = readRequest(request.substr(split + 1));
      if (payload.value("session", std::string{}) != sessionId())
        throw std::runtime_error("The shell restarted; reload the appearance editor");
      if (payload.value("generation", std::uint64_t(-1)) != m_profileGeneration)
        throw std::runtime_error("This appearance preview was replaced; reload before applying");
      if ((action == "commit" || action == "cancel") && !m_profilePreview)
        throw std::runtime_error("This appearance preview is no longer active");
    }
    if (action == "resources") {
      const auto before = text::fontConfigGeneration();
      const auto fonts = text::registerProfileFontResources(payload.at("fonts"));
      const auto effects = registerCustomEffectResources(payload.value("effects", json::array()));
      resources = fonts;
      resources["effects"] = effects.at("effects");
      resources["complete"] = fonts.at("complete").get<bool>() && effects.at("complete").get<bool>();
      if (text::fontConfigGeneration() != before && m_fontResourcesChanged) m_fontResourcesChanged();
    }
    else if (action == "begin") beginProfilePreview();
    else if (action == "commit") {
      noctalia::profile::validateExpected(asJson(config_export::serialize(m_config)), payload.at("expected"));
      if (!commitProfilePreview()) throw std::runtime_error(m_lastMutationError);
    } else if (action == "cancel") {
      noctalia::profile::validateExpected(asJson(config_export::serialize(m_config)), payload.at("expected"));
      cancelProfilePreview();
    }
    else if (action == "external-dirty") {
      if (!payload.at("dirty").is_boolean()) throw std::runtime_error("Expected external appearance dirty flag");
      if (payload.at("dirty").get<bool>()) beginProfilePreview();
      m_profileExternalDirty = payload.at("dirty").get<bool>();
    }
    else if (action == "reconcile" && !m_profilePreview) {
      // Derived background writes cannot reopen a cancelled or saved draft.
    }
    else if (action == "apply" || action == "sync" || action == "reconcile") {
      const auto live = asJson(config_export::serialize(m_config));
      const auto& entries = payload.at("entries");
      const auto& expected = payload.at("expected");
      if (!entries.is_array() || entries.size() > 2048 || !expected.is_array() || expected.size() > 2048)
        throw std::runtime_error("Invalid profile entries");
      if (payload.contains("session") && payload.at("session") != sessionId())
        throw std::runtime_error("The shell restarted; reload the appearance editor");
      auto next = m_overridesTable;
      const bool profilesOnly = action != "sync";
      noctalia::profile::validateExpected(live, expected, profilesOnly);
      for (const auto& entry : entries) {
        const auto path = noctalia::profile::entryPath(entry, profilesOnly);
        const bool guarded = std::ranges::any_of(expected, [&](const auto& before) {
          const auto ancestor = noctalia::profile::entryPath(before, profilesOnly);
          return ancestor.size() <= path.size() && std::equal(ancestor.begin(), ancestor.end(), path.begin());
        });
        if (!guarded) throw std::runtime_error("Every profile change requires an expected value");
      }
      noctalia::profile::applyEntries(next, entries, profilesOnly);
      if (!validateOverrideMutation(next)) throw std::runtime_error(m_lastMutationError);
      const auto resolved = configForOverrides(next);
      if (!resolved) throw std::runtime_error("Cannot resolve profile settings");
      const auto normalized = asJson(config_export::serialize(*resolved));
      for (const auto& entry : entries) {
        const auto path = entry.at("path").get<noctalia::profile::Path>();
        if (!entry.at("value").is_null() && !noctalia::profile::equalValue(
            noctalia::profile::atPath(normalized, path), entry.at("value")))
          throw std::runtime_error("Profile value was rejected or normalized at " + json(path).dump());
      }
      bool changed = false;
      if (!commitOverrideTable(std::move(next), &changed)) throw std::runtime_error(m_lastMutationError);
    } else if (action != "status" && action != "snapshot" && action != "committed"
        && action != "resource-export") {
      throw std::runtime_error("Unknown profile action");
    }
    json result{{"ok", true}, {"session", sessionId()}, {"active", m_profilePreview},
        {"dirty", profilePreviewDirty()}, {"conflict", m_profileConflict}, {"revision", m_profileRevision}, {"generation", m_profileGeneration}};
    if (action == "resources") {
      result["resources"] = resources;
      if (!resources.at("complete").get<bool>()) {
        result["ok"] = false;
        result["error"] = resources.at("error");
      }
    }
    const auto preparation = profilePreparation();
    result["preparation"] = json{{"pending", preparation.pending}, {"error", preparation.error},
        {"skipped", preparation.skipped}};
    const auto receipt = m_persistedOverridesTable["plugin_settings"]["__profile_commit"];
    result["last_commit"] = receipt.is_table() ? asJson(*receipt.as_table()) : json(nullptr);
    if (action == "snapshot") result["config"] = asJson(config_export::serialize(m_config));
    if (action == "resource-export")
      result["resources"] = json{{"effects", exportCustomEffectResources(m_config.shell.materialOverrides)}};
    if (action == "committed") {
      const auto committed = configForOverrides(m_persistedOverridesTable);
      if (!committed) throw std::runtime_error("Cannot resolve committed appearance");
      result["config"] = asJson(config_export::serialize(*committed));
    }
    return result.dump();
  } catch (const std::exception& error) {
    return json{{"ok", false}, {"error", error.what()}}.dump();
  }
}
