#pragma once

#include "config/config_types.h"
#include "scripting/plugin_manager.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Flex;
class Node;

namespace scripting {
  struct PluginManifest;
}

namespace settings {

  class SettingsControlFactory;
  struct SettingEntry;

  // Data + actions for the Plugins settings section. Populated by SettingsWindow
  // from the PluginManager; the section is fully custom (no registry entries).
  struct SettingsPluginsContext {
    float scale = 1.0F;
    std::string_view selectedSection;
    std::vector<scripting::PluginStatus> plugins;
    std::vector<PluginSourceConfig> sources;
    bool searchActive = false;
    Flex* pageTitleRow = nullptr;
    Flex* groupJumpRow = nullptr;
    std::function<void(const Node&)> scrollContentToTop;
    std::unordered_map<std::string, std::unordered_set<std::string>>& expandedGroupsByPage;
    bool pluginsLoading = false;

    std::function<void(std::string id, bool enable)> setEnabled;
    // True while a git-source plugin's runtime export runs in the background; the row
    // shows a spinner in place of the toggle until it lands.
    std::function<bool(const std::string& id)> isEnabling;
    std::function<void()> addSource;
    std::function<void(PluginSourceConfig source, bool enabled)> setSourceEnabled;
    std::function<void(PluginSourceConfig source)> editSource;
    std::function<void(std::string source)> updateSource;
    std::function<void()> refresh;

    // Background auto-update scope for git sources; drives the "auto-update plugins"
    // dropdown. setAutoUpdate persists the mode for all git sources.
    PluginAutoUpdateMode autoUpdateMode = PluginAutoUpdateMode::All;
    std::function<void(PluginAutoUpdateMode)> setAutoUpdate;
    // Update every enabled git source at once (the "update all" action).
    std::function<void()> updateAll;

    // Used to derive current toggle state while async discovery refreshes.
    const Config* config = nullptr;
    std::function<void(std::string id)> onConfigure;
    std::function<void(std::string id)> onRemove;
    std::function<void()> openStore;

    // Plugin id awaiting delete confirmation; its row shows an inline confirm panel.
    std::string pendingDeletePluginId;
    std::function<void(std::string id)> requestDeleteConfirm;
    std::function<void()> cancelDelete;
  };

  // Render the Plugins section into `content` when ctx.selectedSection == "plugins".
  void addSettingsPlugins(Flex& content, SettingsPluginsContext ctx);

  struct PluginSettingsTab {
    std::string id, pluginId, value, label, glyph, nativeSection;
    bool presetManaged = false;
  };
  [[nodiscard]] std::vector<PluginSettingsTab> pluginSettingsTabs(const Config& cfg);
  std::vector<std::string> presetNativeSections(const Config& cfg);
  void addPresetActions(Flex& body, const Config& cfg, SettingsControlFactory& factory,
                        std::string_view section, float scale);
  bool pluginOwnsSetting(const Config& cfg, const std::vector<std::string>& path);

  struct PluginSettingRoute { std::vector<std::string> path; bool value = false; };
  [[nodiscard]] std::optional<PluginSettingRoute> pluginSettingRoute(const Config& cfg, const std::vector<std::string>& path);
  // UI-only routing; the backend's acknowledged native mirror bypasses this adapter.
  bool routePluginSettingWrites(const Config& cfg,
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& writes);
  void routePluginSettingResets(const Config& cfg, std::vector<std::vector<std::string>>& paths);
  void applyPluginSettingRoutes(const Config& cfg, std::vector<SettingEntry>& entries);


  // True when the plugin exposes anything the settings editor can show.
  [[nodiscard]] bool pluginHasSettings(const scripting::PluginManifest& manifest);

  bool buildPluginSettingsEditor(
      Flex& body, const Config& cfg, SettingsControlFactory& factory, const std::string& pluginId,
      const scripting::PluginManifest& manifest, bool showAdvanced, float scale, std::string_view tab = {}
  );

} // namespace settings
