#include "config/config_service.h"
#include "config/profile_scope.h"
#include "core/deferred_call.h"
#include "core/process/process.h"
#include "i18n/i18n.h"
#include "render/text/font_weight_catalog.h"
#include "shell/profile/avatar_path.h"
#include "shell/settings/settings_window.h"
#include "shell/settings/settings_content_plugins.h"
#include "scripting/plugin_registry.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/palette.h"
#include "system/day_night_schedule.h"

#include <algorithm>
#include <chrono>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

  void appendProfileExpected(const nlohmann::json& config, noctalia::profile::Path path,
                             nlohmann::json& entries) {
    for (const auto& [key, value] : config.items()) {
      auto child = path;
      child.push_back(key);
      if (noctalia::profile::owns(child)) entries.push_back({{"path", child}, {"value", value}});
      else if (value.is_object()) appendProfileExpected(value, std::move(child), entries);
    }
  }

  bool settingPathNeedsSceneRebuild(const std::vector<std::string>& path) {
    if (path.size() == 2 && path[0] == "shell") {
      return path[1] == "corner_radius_scale"
          || path[1] == "font_family"
          || path[1] == "lang"
          || path[1] == "settings_window_translucent"
          || path[1] == "settings_compact_chrome" || path[1] == "settings_background";
    }
    if (path.size() == 3 && path[0] == "shell" && path[1] == "design"
        && path[2].starts_with("settings_")) return true;
    if (path.size() == 2 && path[0] == "accessibility") {
      return path[1] == "ui_scale";
    }
    return false;
  }

  bool settingPathsNeedSceneRebuild(const std::vector<std::vector<std::string>>& paths) {
    return std::ranges::any_of(paths, [](const auto& path) { return settingPathNeedsSceneRebuild(path); });
  }

  std::string settingsMutationError(const ConfigService& config, std::string fallback) {
    return config.lastMutationError().empty() ? fallback : config.lastMutationError();
  }

  bool isCustomSchedulePath(const std::vector<std::string>& path) {
    return path.size() == 2
        && path[0] == "location"
        && (path[1] == "custom_schedule" || path[1] == "sunset" || path[1] == "sunrise");
  }

} // namespace

void SettingsWindow::requestPresetSelection(std::string pluginId, std::string selected) {
  if (!m_config || m_profileTransitionBusy || selected.empty()) return;
  if (selected.size() > 256 || selected.find('\0') != std::string::npos) {
    markSettingsWriteError("This preset name is too long or contains invalid characters.");
    return;
  }
  const auto* manifest = scripting::PluginRegistry::instance().findManifest(pluginId);
  if (!manifest || manifest->presetSelection.empty() || manifest->presetCommand.empty()) return;
  const auto& configured = m_config->config().plugins.pluginSettings;
  if (const auto plugin = configured.find(pluginId); plugin != configured.end()) {
    if (const auto setting = plugin->second.find(manifest->presetSelection); setting != plugin->second.end()) {
      if (const auto* current = std::get_if<std::string>(&setting->second); current && *current == selected) return;
    }
  }
  const bool dirty = m_config->profilePreviewDirty();
  showProfilePrompt(selected, false, pluginId);
  if (!dirty) runProfileTransition("discard", std::move(selected), false, std::move(pluginId));
}

void SettingsWindow::showProfilePrompt(std::optional<std::string> selected, bool closeAfter, std::string pluginId) {
  if (!isOpen() || !m_config || m_profileTransitionBusy) return;
  if (m_profileSheetModal && m_profileSheetModal->isOpen()) return;
  dismissOpenSelectDropdown();
  if (!m_profileSheetModal) {
    m_profileSheetModal = std::make_unique<settings::SettingsSheetModal>();
    m_profileSheetModal->initialize(m_modalHost, [this] { dismissOpenSelectDropdown(); });
  }
  const std::weak_ptr<void> alive = m_profileAlive;
  m_profileSheetModal->open(settings::SettingsSheetRequest{
      .sheetTitle = "Unsaved appearance changes",
      .populateSheetBody = [this, alive, selected, closeAfter, pluginId](Flex& body) {
        if (alive.expired()) return;
        const bool conflict = m_config && m_config->profilePreviewConflict();
        const std::string message = m_profileTransitionBusy ? "Applying appearance changes…"
            : conflict ? "Appearance changed outside this editor. Discard to reload the committed settings."
            : selected ? "Save or discard your changes before switching to “" + *selected + "”."
            : closeAfter ? "Save or discard your appearance changes before closing settings."
            : "Save these appearance changes, or discard them to restore the committed settings.";
        body.addChild(settings::makeSettingSubtitleLabel(message, uiScale()));
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * uiScale()});
        row->addChild(ui::spacer());
        auto stay = ui::button({
            .text = "Stay", .enabled = !m_profileTransitionBusy, .variant = ButtonVariant::Ghost,
            .onClick = [this, alive] {
              DeferredCall::callLater([this, alive] {
                if (!alive.expired() && !m_profileTransitionBusy && m_profileSheetModal) m_profileSheetModal->close();
              });
            },
        });
        if (stay->inputArea()) stay->inputArea()->setTabFocusKey("profile-dialog-stay");
        row->addChild(std::move(stay));
        for (const auto& [action, label] : std::vector<std::pair<std::string, std::string>>{
                 {"discard", "Discard"}, {"save", "Save"}}) {
          auto button = ui::button({
              .text = label, .enabled = !m_profileTransitionBusy && !(conflict && action == "save"),
              .variant = action == "save" ? ButtonVariant::Primary : ButtonVariant::Secondary,
              .onClick = [this, alive, action, selected, closeAfter, pluginId] {
                DeferredCall::callLater([this, alive, action, selected, closeAfter, pluginId] {
                  if (!alive.expired()) runProfileTransition(action, selected, closeAfter, pluginId);
                });
              },
          });
          if (button->inputArea()) button->inputArea()->setTabFocusKey("profile-dialog-" + action);
          row->addChild(std::move(button));
        }
        body.addChild(std::move(row));
      },
      .scale = uiScale(), .minWidth = 480.0F, .maxWidth = 680.0F,
      .onCloseRequested = [this] { return m_profileTransitionBusy; },
  });
}

void SettingsWindow::runProfileTransition(std::string action, std::optional<std::string> selected,
                                          bool closeAfter, std::string pluginId) {
  if (!m_config || m_profileTransitionBusy || (action != "save" && action != "discard")) return;
  std::vector<std::string> command;
  bool unacknowledgedProvider = false;
  for (const auto& id : m_config->config().plugins.enabled) {
    if (!pluginId.empty() && id != pluginId) continue;
    const auto* manifest = scripting::PluginRegistry::instance().findManifest(id);
    if (!manifest) continue;
    if (manifest->presetCommand.empty()) {
      unacknowledgedProvider = unacknowledgedProvider || !manifest->presetActions.empty();
      continue;
    }
    if (!command.empty()) {
      finishProfileTransition(false, "More than one enabled plugin owns preset transactions. Open the intended preset editor.", false);
      return;
    }
    command = manifest->presetCommand;
  }
  if (command.empty() && unacknowledgedProvider) {
    finishProfileTransition(false, "The preset plugin does not provide an acknowledged transaction command.", false);
    return;
  }
  m_profileTransitionBusy = true;
  if (m_profileSheetModal) {
    m_profileSheetModal->clearStatusMessage();
    m_profileSheetModal->rebuildBody();
  }
  const std::weak_ptr<void> alive = m_profileAlive;
  if (command.empty()) {
    const auto generation = m_settingsWindowGeneration;
    DeferredCall::callLater([this, alive, generation, action, selected, closeAfter] {
      if (alive.expired() || generation != m_settingsWindowGeneration) return;
      if (selected) {
        finishProfileTransition(false, "No enabled preset transaction command is available.", false);
        return;
      }
      // Cancel restores the native baseline now, then waits for application
      // adapters to acknowledge it. Save waits before making a durable write.
      if (action == "discard") m_config->cancelProfilePreview();
      m_nativeProfileSavePending = action == "save";
      m_nativeProfileCloseAfter = closeAfter;
      onProfilePreparationChanged();
    });
    return;
  }
  try {
    const auto current = nlohmann::json::parse(m_config->profileRequest("status"));
    if (!current.at("ok").get<bool>()) throw std::runtime_error("Cannot read appearance transaction status");
    const auto snapshot = nlohmann::json::parse(m_config->profileRequest("snapshot"));
    if (!snapshot.at("ok").get<bool>()) throw std::runtime_error("Cannot read current appearance settings");
    auto expected = nlohmann::json::array();
    appendProfileExpected(snapshot.at("config"), {}, expected);
    nlohmann::json request{{"action", action},
        {"guard", {{"session", current.at("session")}, {"generation", current.at("generation")},
                   {"expected", std::move(expected)}}}};
    if (selected) request["selected"] = *selected;
    command.push_back(request.dump());
  } catch (const std::exception& error) {
    finishProfileTransition(false, error.what(), false);
    return;
  }
  const auto generation = m_settingsWindowGeneration;
  const bool launched = process::runAsync(command, process::RunCallbacks{
      .onExit = [this, alive, generation, closeAfter](process::RunResult result) {
        DeferredCall::callLater([this, alive, generation, closeAfter, result = std::move(result)] {
          if (alive.expired() || generation != m_settingsWindowGeneration) return;
          bool success = false;
          std::string error = result.timedOut ? "The appearance operation timed out. Review the current settings before retrying."
              : "The preset backend did not acknowledge the operation.";
          try {
            const auto response = nlohmann::json::parse(result.out);
            success = static_cast<bool>(result) && !result.outTruncated && response.value("ok", false);
            if (!success && response.contains("error") && response["error"].is_string())
              error = response["error"].get<std::string>().substr(0, 2048);
          } catch (const std::exception&) {}
          finishProfileTransition(success, std::move(error), closeAfter);
        });
      },
  }, process::RunOptions{.timeout = std::chrono::seconds(60), .maxOutputBytes = 65536});
  if (!launched) finishProfileTransition(false, "Cannot start the preset transaction command.", false);
}

void SettingsWindow::onProfilePreparationChanged() {
  onExternalOptionsChanged();
  if (!m_nativeProfileSavePending || !m_config) return;
  const std::weak_ptr<void> alive = m_profileAlive;
  const auto generation = m_settingsWindowGeneration;
  DeferredCall::callLater([this, alive, generation] {
    if (alive.expired() || generation != m_settingsWindowGeneration
        || !m_nativeProfileSavePending || !m_config) return;
    const auto preparation = m_config->profilePreparation();
    if (preparation.pending) return; // Resumed by the adapter completion event.
    const bool save = *m_nativeProfileSavePending;
    const bool closeAfter = m_nativeProfileCloseAfter;
    m_nativeProfileSavePending.reset();
    if (!preparation.error.empty()) {
      finishProfileTransition(false, preparation.error, false);
      return;
    }
    const bool success = !save || m_config->commitProfilePreview();
    finishProfileTransition(success, m_config->lastMutationError(), closeAfter);
  });
}

void SettingsWindow::finishProfileTransition(bool success, std::string error, bool closeAfter) {
  m_profileTransitionBusy = false;
  if (success && m_config) m_config->checkReload();
  if (success && closeAfter && m_config && m_config->profilePreviewDirty()) {
    success = false;
    error = "Appearance still has unsaved changes. Settings will remain open.";
  }
  if (!success) {
    if (error.empty()) error = "Cannot complete the appearance operation.";
    if (m_profileSheetModal && m_profileSheetModal->isOpen()) {
      m_profileSheetModal->setStatusMessage(std::move(error), true);
      m_profileSheetModal->rebuildBody();
    } else markSettingsWriteError(std::move(error));
    return;
  }
  markSettingsWriteSuccess(false);
  if (m_profileSheetModal) m_profileSheetModal->close();
  if (closeAfter) {
    destroyWindow();
    return;
  }
  requestContentRebuild(true, true);
}

// Custom scheduling with unusable times schedules nothing. The write itself is valid, so surface it
// as a banner rather than rejecting it — the user is likely mid-edit between the toggle and the times.
void SettingsWindow::warnOnUnusableCustomSchedule(const std::vector<std::string>& path) {
  if (m_config == nullptr || !isCustomSchedulePath(path)) {
    return;
  }
  const LocationConfig& location = m_config->config().location;
  if (location.customSchedule && !day_night_schedule::hasUsableCustomTimes(location)) {
    showTransientStatus(i18n::tr("settings.errors.custom-schedule-times"), true);
  }
}

void SettingsWindow::markSettingsWriteSuccess(bool requestRebuild) {
  if (m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
    m_editorSheetModal->clearStatusMessage();
  }
  m_statusMessage.clear();
  m_statusIsError = false;
  m_pendingResetPageScope.clear();
  m_pendingResetSettingPaths.clear();
  if (requestRebuild) {
    requestSceneRebuild();
  }
}

void SettingsWindow::markSettingsWriteError(std::string message) {
  if (m_editorSheetModal != nullptr && m_editorSheetModal->isOpen()) {
    m_editorSheetModal->setStatusMessage(std::move(message), true);
    return;
  }
  m_statusMessage = std::move(message);
  m_statusIsError = true;
  requestSceneRebuild();
}

void SettingsWindow::finishSettingsWrite(
    bool changed, bool forceSceneRebuild, bool pageResetPathsChanged, bool registryAlreadyCurrent,
    bool rebuildWhenUnchanged
) {
  const bool hadStatus = !m_statusMessage.empty();
  const bool hadPendingReset = !m_pendingResetPageScope.empty() || !m_pendingResetSettingPaths.empty();
  markSettingsWriteSuccess(false);
  if (forceSceneRebuild || hadStatus || hadPendingReset) {
    requestSceneRebuild();
  } else if (changed || rebuildWhenUnchanged) {
    requestContentRebuild(
        changed ? !registryAlreadyCurrent : true, pageResetPathsChanged || rebuildWhenUnchanged, true
    );
  }
}

void SettingsWindow::showTransientStatus(std::string message, bool isError) {
  m_statusMessage = std::move(message);
  m_statusIsError = isError;
  requestSceneRebuild();
}

void SettingsWindow::deferSettingsMutation(std::function<void()> mutation) {
  const std::weak_ptr<void> alive = m_profileAlive;
  const auto generation = m_settingsWindowGeneration;
  DeferredCall::callLater([this, alive, generation, mutation = std::move(mutation)]() mutable {
    // Test the independent lifetime token before reading any member through
    // this. A still-alive controller may already own a newly opened window.
    if (alive.expired() || generation != m_settingsWindowGeneration) return;
    mutation();
  });
}

void SettingsWindow::setSettingOverride(std::vector<std::string> path, ConfigOverrideValue value) {
  if (path.size() == 2 && path[0] == "shell" && path[1] == "font_family") {
    text::invalidateFontWeightCatalogCache();
  }
  const bool isAvatarPath = path.size() == 2 && path[0] == "shell" && path[1] == "avatar_path";
  deferSettingsMutation([this, path = std::move(path), value = std::move(value), isAvatarPath]() mutable {
    if (m_config == nullptr) {
      return;
    }
    if (isAvatarPath) {
      const auto* avatarPath = std::get_if<std::string>(&value);
      if (avatarPath == nullptr) {
        markSettingsWriteError(i18n::tr("settings.errors.write"));
        return;
      }
      const auto result = shell::applyAvatarPath(m_accounts, m_config, *avatarPath);
      if (result.success()) {
        markSettingsWriteSuccess();
        return;
      }
      markSettingsWriteError(i18n::tr(shell::avatarApplyErrorTranslationKey(result.error)));
      return;
    }
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> routed{{path, value}};
    if (!settings::routePluginSettingWrites(m_config->config(), routed)) {
      markSettingsWriteError("Conflicting or invalid owned setting values");
      return;
    }
    path = std::move(routed.front().first);
    value = std::move(routed.front().second);
    bool changed = false;
    const bool needsSceneRebuild = settingPathNeedsSceneRebuild(path);
    const ConfigOverrideValue patchValue = value;
    const auto previousResetPaths = currentPageResetPaths();
    if (m_config->setOverride(path, std::move(value), &changed)) {
      const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryValue(path, patchValue);
      finishSettingsWrite(changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched);
      warnOnUnusableCustomSchedule(path);
      return;
    }
    markSettingsWriteError(settingsMutationError(*m_config, i18n::tr("settings.errors.write")));
  });
}

void SettingsWindow::setSettingOverrides(
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides
) {
  deferSettingsMutation([this, overrides = std::move(overrides)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    if (overrides.empty()) {
      markSettingsWriteSuccess(!m_statusMessage.empty());
      return;
    }
    if (!settings::routePluginSettingWrites(m_config->config(), overrides)) {
      markSettingsWriteError("Conflicting or invalid owned setting values");
      return;
    }
    bool changed = false;
    const bool needsSceneRebuild = std::ranges::any_of(overrides, [](const auto& overrideEntry) {
      return settingPathNeedsSceneRebuild(overrideEntry.first);
    });
    const auto patchOverrides = overrides;
    const auto previousResetPaths = currentPageResetPaths();
    if (m_config->setOverrides(std::move(overrides), &changed)) {
      const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryOverrides(patchOverrides);
      finishSettingsWrite(changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched);
      return;
    }
    markSettingsWriteError(settingsMutationError(*m_config, i18n::tr("settings.errors.batch-write")));
  });
}

void SettingsWindow::clearSettingOverride(std::vector<std::string> path) {
  deferSettingsMutation([this, path = std::move(path)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    if (const auto route = settings::pluginSettingRoute(m_config->config(), path)) path = route->path;
    bool changed = false;
    const bool needsSceneRebuild = settingPathNeedsSceneRebuild(path);
    const auto previousResetPaths = currentPageResetPaths();
    if (!m_config->clearOverrides({path}, &changed)) {
      markSettingsWriteError(i18n::tr("settings.errors.write"));
      return;
    }

    const std::vector<std::vector<std::string>> paths{path};
    const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryResetValues(paths);
    finishSettingsWrite(
        changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched, true
    );
  });
}

void SettingsWindow::clearSettingOverrides(std::vector<std::vector<std::string>> paths) {
  deferSettingsMutation([this, paths = std::move(paths)]() mutable {
    if (m_config == nullptr || paths.empty()) {
      return;
    }

    settings::routePluginSettingResets(m_config->config(), paths);
    bool changed = false;
    const bool needsSceneRebuild = settingPathsNeedSceneRebuild(paths);
    const auto previousResetPaths = currentPageResetPaths();
    const bool success = m_config->clearOverrides(paths, &changed);
    m_pendingResetPageScope.clear();
    if (!success) {
      markSettingsWriteError(i18n::tr("settings.errors.reset-page"));
      return;
    }

    const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryResetValues(paths);
    finishSettingsWrite(
        changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched, true
    );
  });
}

void SettingsWindow::resetBarLane(std::vector<std::string> lanePath) {
  deferSettingsMutation([this, lanePath = std::move(lanePath)]() mutable {
    if (m_config == nullptr) {
      return;
    }
    bool changed = false;
    const bool needsSceneRebuild = settingPathNeedsSceneRebuild(lanePath);
    const auto previousResetPaths = currentPageResetPaths();
    if (!m_config->resetBarLaneOverride(lanePath, &changed)) {
      markSettingsWriteError(i18n::tr("settings.errors.write"));
      return;
    }

    const std::vector<std::vector<std::string>> paths{lanePath};
    const bool registryPatched = changed && !needsSceneRebuild && tryPatchSettingsRegistryResetValues(paths);
    finishSettingsWrite(
        changed, needsSceneRebuild, previousResetPaths != currentPageResetPaths(), registryPatched, true
    );
  });
}

void SettingsWindow::renameWidgetInstance(
    std::string oldName, std::string newName,
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> referenceOverrides
) {
  deferSettingsMutation([this, oldName = std::move(oldName), newName = std::move(newName),
                           referenceOverrides = std::move(referenceOverrides)]() mutable {
    if (m_config == nullptr) {
      return;
    }

    bool changed = m_config->renameOverrideTable({"widget", oldName}, {"widget", newName});
    if (!changed) {
      markSettingsWriteError(i18n::tr("settings.errors.widget.rename"));
      return;
    }
    bool failed = false;
    for (auto& [path, value] : referenceOverrides) {
      if (m_config->setOverride(path, std::move(value))) {
        changed = true;
      } else {
        failed = true;
      }
    }
    if (failed) {
      markSettingsWriteError(i18n::tr("settings.errors.batch-write"));
      return;
    }
    markSettingsWriteSuccess(changed);
  });
}

void SettingsWindow::createBar(std::string name) {
  deferSettingsMutation([this, name = std::move(name)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createBarOverride(name)) {
      m_selectedSection = "bar";
      m_selectedBarName = name;
      m_selectedMonitorOverride.clear();
      m_creatingBarName.clear();
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.create"));
  });
}

void SettingsWindow::renameBar(std::string oldName, std::string newName) {
  deferSettingsMutation([this, oldName = std::move(oldName), newName = std::move(newName)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameBarOverride(oldName, newName)) {
      if (m_selectedBarName == oldName) {
        m_selectedBarName = newName;
      }
      m_selectedMonitorOverride.clear();
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.rename"));
  });
}

void SettingsWindow::deleteBar(std::string name) {
  deferSettingsMutation([this, name = std::move(name)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteBarOverride(name)) {
      if (m_selectedBarName == name) {
        m_selectedBarName.clear();
        m_selectedMonitorOverride.clear();
        m_contentScrollState.offset = 0.0F;
      }
      m_renamingBarName.clear();
      m_pendingDeleteBarName.clear();
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.delete"));
  });
}

void SettingsWindow::moveBar(std::string name, int direction) {
  deferSettingsMutation([this, name = std::move(name), direction]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->moveBarOverride(name, direction)) {
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.bar.move"));
  });
}

void SettingsWindow::createMonitorOverride(std::string barName, std::string match) {
  deferSettingsMutation([this, barName = std::move(barName), match = std::move(match)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->createMonitorOverride(barName, match)) {
      m_selectedSection = "bar";
      m_selectedBarName = barName;
      m_selectedMonitorOverride = match;
      m_creatingMonitorOverrideBarName.clear();
      m_creatingMonitorOverrideMatch.clear();
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.create"));
  });
}

void SettingsWindow::renameMonitorOverride(std::string barName, std::string oldMatch, std::string newMatch) {
  deferSettingsMutation([this, barName = std::move(barName), oldMatch = std::move(oldMatch),
                           newMatch = std::move(newMatch)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->renameMonitorOverride(barName, oldMatch, newMatch)) {
      if (m_selectedBarName == barName && m_selectedMonitorOverride == oldMatch) {
        m_selectedMonitorOverride = newMatch;
      }
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      m_contentScrollState.offset = 0.0F;
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.rename"));
  });
}

void SettingsWindow::deleteMonitorOverride(std::string barName, std::string match) {
  deferSettingsMutation([this, barName = std::move(barName), match = std::move(match)]() {
    if (m_config == nullptr) {
      return;
    }
    if (m_config->deleteMonitorOverride(barName, match)) {
      if (m_selectedBarName == barName && m_selectedMonitorOverride == match) {
        m_selectedMonitorOverride.clear();
        m_contentScrollState.offset = 0.0F;
      }
      m_renamingMonitorOverrideBarName.clear();
      m_renamingMonitorOverrideMatch.clear();
      m_pendingDeleteMonitorOverrideBarName.clear();
      m_pendingDeleteMonitorOverrideMatch.clear();
      markSettingsWriteSuccess();
      return;
    }
    markSettingsWriteError(i18n::tr("settings.errors.monitor-override.delete"));
  });
}
