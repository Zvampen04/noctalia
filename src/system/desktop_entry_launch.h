#pragma once

#include "system/desktop_entry.h"

#include <gio/gio.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct DesktopAction;

namespace desktop_entry_launch {

  struct LaunchOptions {
    std::string activationToken;
    bool runAsSystemdService = false;
    std::string customCommand;
    // When true, try org.freedesktop.Application Activate/ActivateAction before Exec.
    bool dbusActivatable = false;
    // Desktop-file id (stem) used as the D-Bus well-known name; required when dbusActivatable.
    std::string dbusAppId;
    // Empty → Activate; otherwise ActivateAction(action_name, ...).
    std::string desktopActionId;
  };

  struct PrepareOptions {
    std::vector<std::string> terminalCandidates;
    bool useSystemTerminalDiscovery = true;
  };

  struct PreparedCommand {
    std::vector<std::string> args;
  };

  [[nodiscard]] std::optional<PreparedCommand>
  prepareCommand(std::string_view exec, bool terminal, const PrepareOptions& options = {});

  // Launches the configured MIME handler without passing a file or URI.
  [[nodiscard]] bool launchDefaultForMimeType(std::string_view mimeType);
  // Retain the default handler's Exec, Terminal, Path and D-Bus metadata when
  // routing it through the same application policy as launcher entries.
  [[nodiscard]] std::optional<DesktopEntry> desktopEntryForAppInfo(GAppInfo* appInfo);

  [[nodiscard]] bool launchEntry(const DesktopEntry& entry, const LaunchOptions& options = {});

  [[nodiscard]] bool launchAction(
      const DesktopAction& action, std::string_view appName, std::string_view workingDir, bool terminal,
      const LaunchOptions& options = {}
  );

} // namespace desktop_entry_launch
