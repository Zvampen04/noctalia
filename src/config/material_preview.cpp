#include "config/config_service.h"
#include "config/schema/config_schema.h"
#include "config/schema/engine.h"
#include <algorithm>

std::optional<bool> ConfigService::previewMaterialScalars(
    const std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides, bool* changed) {
  if (!m_profileTransactionsReady || m_profileCommitting || !m_overridesParseError.empty() || overrides.empty())
    return std::nullopt;
  // Only independent numeric material fields use this path. Structural edits,
  // effect imports, resets and Save retain complete transaction validation.
  for (const auto& [path, value] : overrides) {
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
    if (field.key == "material" || field.key == "material_overrides") field.write(shell, m_config.shell);
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
    else table->insert_or_assign(path.back(), std::get<std::int64_t>(value));
    return true;
  };
  for (const auto& [path, value] : overrides)
    if (!insert(effective, path, value) || !insert(next, path, value)) return std::nullopt;
  if (next == m_overridesTable) { m_lastMutationError.clear(); return true; }
  auto updated = m_config;
  schema::Diagnostics diagnostics;
  schema::readInto(*effective.get_as<toml::table>("shell"), updated.shell, schema::shellSchema(), "shell", diagnostics);
  if (diagnostics.hasErrors()) {
    m_lastMutationError = diagnostics.entries.front().describeShort(m_configDir);
    return false;
  }
  beginProfilePreview();
  m_overridesTable = std::move(next);
  m_lastChange = computeConfigChangeSet(m_config, updated);
  m_config = std::move(updated);
  ++m_profileRevision;
  m_lastMutationError.clear();
  if (changed) *changed = true;
  m_materialPreviewUpdate = true;
  try { fireReloadCallbacks(); }
  catch (...) { m_materialPreviewUpdate = false; throw; }
  m_materialPreviewUpdate = false;
  return true;
}
