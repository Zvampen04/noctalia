#include "config/config_service.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include "config/config_validate.h"
#include "scripting/plugin_registry.h"
#include <algorithm>
#include <cmath>

std::optional<bool> ConfigService::previewContinuousSettings(
    const std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides, bool* changed) {
  if (!m_profileTransactionsReady || m_profileCommitting || !m_overridesParseError.empty() || overrides.empty())
    return std::nullopt;
  // Only scalar material fields and declared animation curves use this path. Structural edits,
  // effect imports, resets and Save retain complete transaction validation.
  for (const auto& [path, value] : overrides) {
    if (path.size() == 3 && path[0] == "shell" && path[1] == "animation") {
      if (path[2] == "style" && std::holds_alternative<std::string>(value) &&
          parseMotionStyle(std::get<std::string>(value))) continue;
      if (std::holds_alternative<double>(value)) {
        const auto number = std::get<double>(value);
        const bool x = path[2] == "curve_x1" || path[2] == "curve_x2";
        const bool y = path[2] == "curve_y1" || path[2] == "curve_y2";
        if (std::isfinite(number) && ((x && number >= 0 && number <= 1) ||
            (y && number >= -2 && number <= 2))) continue;
      }
      return std::nullopt;
    }
    if (path.size() == 3 && path[0] == "plugin_settings") {
      const auto* manifest = scripting::PluginRegistry::instance().findManifest(path[1]);
      bool curveField = false;
      if (manifest) for (const auto& field : manifest->settings) if (field.curve) {
        const auto& curve = *field.curve;
        curveField = curveField || std::ranges::find(curve.keys, path[2]) != curve.keys.end()
            || (!curve.activationKey.empty() && curve.activationKey == path[2]);
      }
      if (curveField && (std::holds_alternative<double>(value) ||
          std::holds_alternative<std::int64_t>(value) || std::holds_alternative<std::string>(value))) continue;
      return std::nullopt;
    }
    const bool global = path.size() == 3 && path[0] == "shell" && path[1] == "material";
    const bool scoped = path.size() == 5 && path[0] == "shell" && path[1] == "material_overrides"
        && (path[2] == "roles" || path[2] == "families" || path[2] == "surfaces")
        && Style::validMaterialTarget(path[3]);
    if (!global && !scoped) return std::nullopt;
    bool scalar = false;
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) \
    scalar = scalar || path.back() == #key;
#include "material/fields.def"
#undef MATERIAL_FIELD
    if (!scalar || (!std::holds_alternative<double>(value) && !std::holds_alternative<std::int64_t>(value)))
      return std::nullopt;
  }
  namespace schema = noctalia::config::schema;
  toml::table shell;
  for (const auto& field : schema::shellSchema())
    if (field.key == "material" || field.key == "material_overrides" || field.key == "animation")
      field.write(shell, m_config.shell);
  toml::table effective{{"shell", std::move(shell)}};
  auto next = m_overridesTable;
  const auto insert = [](toml::table& root, const std::vector<std::string>& path, const ConfigOverrideValue& value) {
    auto* table = &root;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      if (!table->contains(path[i])) table->insert(path[i], toml::table{});
      table = table->get_as<toml::table>(path[i]);
      if (!table) return false;
    }
    if (const auto* number = std::get_if<double>(&value)) table->insert_or_assign(path.back(), *number);
    else if (const auto* number = std::get_if<std::int64_t>(&value)) table->insert_or_assign(path.back(), *number);
    else table->insert_or_assign(path.back(), std::get<std::string>(value));
    return true;
  };
  for (const auto& [path, value] : overrides)
    if (!insert(effective, path, value) || !insert(next, path, value)) return std::nullopt;
  if (next == m_overridesTable) { m_lastMutationError.clear(); return true; }
  auto updated = m_config;
  schema::Diagnostics diagnostics;
  schema::readInto(*effective.get_as<toml::table>("shell"), updated.shell, schema::shellSchema(), "shell", diagnostics);
  const auto pluginDiagnostics = noctalia::config::validatePluginPreview(effective);
  diagnostics.entries.insert(diagnostics.entries.end(), pluginDiagnostics.entries.begin(), pluginDiagnostics.entries.end());
  if (diagnostics.hasErrors()) {
    m_lastMutationError = diagnostics.entries.front().describeShort(m_configDir);
    return false;
  }
  for (const auto& [path, value] : overrides) if (path[0] == "plugin_settings") {
    auto& destination = updated.plugins.pluginSettings[path[1]][path[2]];
    if (const auto* number = std::get_if<double>(&value)) destination = *number;
    else if (const auto* number = std::get_if<std::int64_t>(&value)) destination = *number;
    else destination = std::get<std::string>(value);
  }
  beginProfilePreview();
  m_overridesTable = std::move(next);
  m_lastChange = computeConfigChangeSet(m_config, updated);
  m_config = std::move(updated);
  ++m_profileRevision;
  m_lastMutationError.clear();
  if (changed) *changed = true;
  m_continuousPreviewUpdate = true;
  try { fireReloadCallbacks(); }
  catch (...) { m_continuousPreviewUpdate = false; throw; }
  m_continuousPreviewUpdate = false;
  return true;
}
