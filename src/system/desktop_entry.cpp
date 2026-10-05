#include "system/desktop_entry.h"

#include "core/inotify/inotify.h"
#include "core/log.h"
#include "i18n/language_tag.h"
#include "system/desktop_entry_cache.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

  constexpr Logger kLog("desktop_entry");
  using noctalia::detail::DesktopEntryEnvironment;

  bool isUserDesktopEntry(const fs::path& filepath, const DesktopEntryEnvironment& environment) {
    fs::path dataHome;
    if (!environment.dataHome.empty()) {
      dataHome = environment.dataHome;
    } else if (!environment.home.empty()) {
      dataHome = fs::path(environment.home) / ".local/share";
    } else {
      return false;
    }

    const fs::path applications = (dataHome / "applications").lexically_normal();
    const fs::path normalized = filepath.lexically_normal();
    const auto mismatch = std::ranges::mismatch(applications, normalized);
    return mismatch.in1 == applications.end();
  }

  fs::path resolveExecutable(std::string_view executable, const DesktopEntryEnvironment& environment) {
    const fs::path path(executable);
    if (path.has_parent_path()) {
      return path;
    }

    if (environment.path.empty()) {
      return {};
    }

    const std::string_view searchPath(environment.path);
    std::size_t start = 0;
    while (start <= searchPath.size()) {
      const std::size_t end = searchPath.find(':', start);
      const std::string_view directory =
          end == std::string_view::npos ? searchPath.substr(start) : searchPath.substr(start, end - start);
      const fs::path candidate = directory.empty() ? path : fs::path(directory) / path;
      std::error_code ec;
      if (::access(candidate.c_str(), X_OK) == 0 && fs::is_regular_file(candidate, ec)) {
        return candidate;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return {};
  }

  bool executableIsAppImage(std::string_view exec, bool resolveFromPath, const DesktopEntryEnvironment& environment) {
    const auto start = exec.find_first_not_of(' ');
    if (start == std::string_view::npos) {
      return false;
    }

    const char quote = exec[start] == '"' ? '"' : '\0';
    const std::size_t executableStart = start + (quote == '\0' ? 0 : 1);
    const std::size_t executableEnd =
        quote == '\0' ? exec.find(' ', executableStart) : exec.find(quote, executableStart);
    const fs::path executable(exec.substr(
        executableStart,
        executableEnd == std::string_view::npos ? std::string_view::npos : executableEnd - executableStart
    ));
    if (StringUtils::toLower(executable.extension().string()) == ".appimage") {
      return true;
    }

    const fs::path resolved = executable.has_parent_path()
        ? executable
        : (resolveFromPath ? resolveExecutable(executable.string(), environment) : fs::path{});
    std::ifstream file(resolved, std::ios::binary);
    std::array<char, 10> header{};
    if (!file.read(header.data(), static_cast<std::streamsize>(header.size()))) {
      return false;
    }
    // AppImage reserves these two ELF identification bytes for its format marker.
    return header[0] == '\x7F'
        && header[1] == 'E'
        && header[2] == 'L'
        && header[3] == 'F'
        && header[8] == 'A'
        && header[9] == 'I';
  }

  DesktopEntryOrigin detectOrigin(const fs::path& filepath, bool appImage, const DesktopEntryEnvironment& environment) {
    const std::string path = filepath.lexically_normal().string();
    if (path.contains("/flatpak/") && path.contains("/exports/share/applications/")) {
      return DesktopEntryOrigin::Flatpak;
    }
    if (path.contains("/snap/") || path.contains("/snapd/desktop/applications/")) {
      return DesktopEntryOrigin::Snap;
    }
    if (path.contains("/nix/store/")) {
      return DesktopEntryOrigin::Nix;
    }
    if (appImage) {
      return DesktopEntryOrigin::AppImage;
    }
    if (!environment.home.empty()) {
      const std::string userApplications = environment.home + "/.local/share/applications/";
      if (path.starts_with(userApplications)) {
        return DesktopEntryOrigin::User;
      }
    }
    return DesktopEntryOrigin::System;
  }

  bool parseDesktopBool(std::string_view value) {
    const std::string lower = StringUtils::toLower(value);
    return lower == "true" || lower == "1" || lower == "yes";
  }

  void splitMultipleDesktopStrings(std::vector<std::string>& parsedValues, std::string_view fullValue) {
    std::size_t start = 0;
    while (start < fullValue.size()) {
      const auto delimiter = fullValue.find(';', start);
      const auto token =
          (delimiter == std::string_view::npos) ? fullValue.substr(start) : fullValue.substr(start, delimiter - start);
      if (!token.empty()) {
        parsedValues.emplace_back(token);
      }
      if (delimiter == std::string_view::npos) {
        break;
      }
      start = delimiter + 1;
    }
  }

  bool shouldShowOnCurrentDesktop(
      const std::vector<std::string>& onlyShowIn, const std::vector<std::string>& notShowIn,
      const DesktopEntryEnvironment& environment
  ) {
    const auto visibleByDefault = onlyShowIn.empty();
    if (environment.currentDesktop.empty()) {
      return visibleByDefault;
    }
    std::string_view desktops(environment.currentDesktop);
    std::size_t start = 0;
    while (start <= desktops.size()) {
      const auto delimiter = desktops.find(':', start);
      const auto token =
          (delimiter == std::string_view::npos) ? desktops.substr(start) : desktops.substr(start, delimiter - start);
      if (!token.empty()) {
        if (std::ranges::find(onlyShowIn, token) != onlyShowIn.end()) {
          return true;
        } else if (std::ranges::find(notShowIn, token) != notShowIn.end()) {
          return false;
        }
      }
      if (delimiter == std::string_view::npos) {
        break;
      }
      start = delimiter + 1;
    }
    return visibleByDefault;
  }

  using LocalizedValues = std::unordered_map<std::string, std::string>;

  struct LocalizedAssignment {
    std::string_view key;
    std::string locale;
    std::string_view value;
  };

  std::string normalizeDesktopLocale(std::string_view rawLocale) {
    const auto modifierStart = rawLocale.find('@');
    const std::string_view base = rawLocale.substr(0, modifierStart);
    std::string locale = i18n::detail::normalizeLanguageTag(base);
    if (locale.empty() || modifierStart == std::string_view::npos) {
      return locale;
    }

    const std::string_view modifier = rawLocale.substr(modifierStart + 1);
    if (modifier.empty()) {
      return {};
    }
    locale += '@';
    for (const unsigned char character : modifier) {
      locale += static_cast<char>(std::tolower(character));
    }
    return locale;
  }

  std::optional<LocalizedAssignment> parseLocalizedAssignment(std::string_view line) {
    const auto equals = line.find('=');
    if (equals == std::string_view::npos) {
      return std::nullopt;
    }

    const std::string_view fullKey = line.substr(0, equals);
    const auto bracket = fullKey.find('[');
    if (bracket == std::string_view::npos || bracket == 0 || fullKey.back() != ']') {
      return std::nullopt;
    }

    const std::string_view rawLocale = fullKey.substr(bracket + 1, fullKey.size() - bracket - 2);
    std::string locale = normalizeDesktopLocale(rawLocale);
    if (locale.empty()) {
      return std::nullopt;
    }

    return LocalizedAssignment{
        .key = fullKey.substr(0, bracket),
        .locale = std::move(locale),
        .value = line.substr(equals + 1),
    };
  }

  std::string localizedValue(std::string_view language, const LocalizedValues& values, std::string_view defaultValue) {
    for (const std::string& candidate : i18n::detail::catalogLanguageCandidates(language)) {
      if (const auto it = values.find(candidate); it != values.end()) {
        return it->second;
      }
    }
    return std::string(defaultValue);
  }

  void parseDesktopFile(
      const fs::path& filepath, std::string_view language, const DesktopEntryEnvironment& environment,
      std::vector<DesktopEntry>& entries
  ) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
      kLog.debug("failed to open desktop entry file '{}'", filepath.string());
      return;
    }

    DesktopEntry entry;
    entry.path = filepath.string();
    entry.id = filepath.stem().string();

    bool inDesktopEntry = false;
    bool inAction = false;
    LocalizedValues localizedNames;
    LocalizedValues localizedGenericNames;
    LocalizedValues localizedComments;
    LocalizedValues localizedKeywords;
    std::string type;
    bool hasAppImageMetadata = false;

    // Desktop-environment visibility lists (OnlyShowIn/NotShowIn)
    std::vector<std::string> onlyShowIn;
    std::vector<std::string> notShowIn;

    // Action parsing state
    std::vector<std::string> actionOrder;
    struct ActionData {
      std::string name;
      std::string exec;
      LocalizedValues localizedNames;
    };
    std::unordered_map<std::string, ActionData> actionMap;
    std::string currentActionId;
    ActionData currentActionData;

    auto flushCurrentAction = [&]() {
      if (!currentActionId.empty()) {
        currentActionData.name = localizedValue(language, currentActionData.localizedNames, currentActionData.name);
        if (!currentActionData.name.empty() && !currentActionData.exec.empty()) {
          actionMap[currentActionId] = currentActionData;
        }
        currentActionId.clear();
        currentActionData = {};
      }
    };

    std::string line;
    while (std::getline(file, line)) {
      // Strip trailing whitespace/carriage return
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
      }

      if (line.empty() || line[0] == '#') {
        continue;
      }

      if (line[0] == '[') {
        flushCurrentAction();
        inDesktopEntry = false;
        inAction = false;

        if (line == "[Desktop Entry]") {
          inDesktopEntry = true;
        } else if (line.size() > 17 && line.starts_with("[Desktop Action ") && line.back() == ']') {
          currentActionId = line.substr(16, line.size() - 17);
          if (!currentActionId.empty()) {
            inAction = true;
          }
        }
        continue;
      }

      if (inAction) {
        if (const auto localized = parseLocalizedAssignment(line); localized && localized->key == "Name") {
          currentActionData.localizedNames.insert_or_assign(localized->locale, localized->value);
          continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
          continue;
        std::string_view key(line.data(), eq);
        std::string_view value(line.data() + eq + 1, line.size() - eq - 1);
        if (key == "Name") {
          currentActionData.name = std::string(value);
        } else if (key == "Exec") {
          currentActionData.exec = std::string(value);
        }
        continue;
      }

      if (!inDesktopEntry) {
        continue;
      }

      if (const auto localized = parseLocalizedAssignment(line)) {
        LocalizedValues* values = nullptr;
        if (localized->key == "Name") {
          values = &localizedNames;
        } else if (localized->key == "GenericName") {
          values = &localizedGenericNames;
        } else if (localized->key == "Comment") {
          values = &localizedComments;
        } else if (localized->key == "Keywords") {
          values = &localizedKeywords;
        }
        if (values != nullptr) {
          values->insert_or_assign(localized->locale, localized->value);
        }
        continue;
      }

      auto eq = line.find('=');
      if (eq == std::string::npos) {
        continue;
      }

      std::string_view key(line.data(), eq);
      std::string_view value(line.data() + eq + 1, line.size() - eq - 1);

      if (key == "Type") {
        type = std::string(value);
      } else if (key == "Name") {
        entry.name = std::string(value);
      } else if (key == "GenericName") {
        entry.genericName = std::string(value);
      } else if (key == "Comment") {
        entry.comment = std::string(value);
      } else if (key == "Exec") {
        entry.exec = std::string(value);
      } else if (key == "Icon") {
        entry.icon = std::string(value);
      } else if (key == "Categories") {
        entry.categories = std::string(value);
      } else if (key == "Keywords") {
        entry.keywords = std::string(value);
      } else if (key == "StartupWMClass") {
        entry.startupWmClass = std::string(value);
      } else if (key == "NoDisplay") {
        entry.noDisplay = parseDesktopBool(value);
      } else if (key == "Hidden") {
        entry.hidden = parseDesktopBool(value);
      } else if (key == "Path") {
        entry.workingDir = std::string(value);
      } else if (key == "Terminal") {
        entry.terminal = parseDesktopBool(value);
      } else if (key == "DBusActivatable") {
        entry.dbusActivatable = parseDesktopBool(value);
      } else if (key == "OnlyShowIn") {
        splitMultipleDesktopStrings(onlyShowIn, value);
      } else if (key == "NotShowIn") {
        splitMultipleDesktopStrings(notShowIn, value);
      } else if (key == "Actions") {
        splitMultipleDesktopStrings(actionOrder, value);
      } else if (key.starts_with("X-AppImage-")) {
        hasAppImageMetadata = true;
      }
    }

    // Flush any trailing action section.
    flushCurrentAction();

    if (type != "Application"
        || entry.noDisplay
        || entry.hidden
        || entry.name.empty()
        || !shouldShowOnCurrentDesktop(onlyShowIn, notShowIn, environment)) {
      return;
    }

    const std::string defaultName = entry.name;
    entry.name = localizedValue(language, localizedNames, entry.name);
    entry.genericName = localizedValue(language, localizedGenericNames, entry.genericName);
    entry.comment = localizedValue(language, localizedComments, entry.comment);
    entry.keywords = localizedValue(language, localizedKeywords, entry.keywords);

    entry.localizedNamesLower.reserve(localizedNames.size() + 1);
    auto appendNameAlias = [&](std::string_view name) {
      const std::string lower = StringUtils::toLower(name);
      if (!lower.empty()
          && lower != StringUtils::toLower(entry.name)
          && !std::ranges::contains(entry.localizedNamesLower, lower)) {
        entry.localizedNamesLower.push_back(lower);
      }
    };
    appendNameAlias(defaultName);
    for (const auto& [_, name] : localizedNames) {
      appendNameAlias(name);
    }

    // Pre-lowercase for matching
    entry.nameLower = StringUtils::toLower(entry.name);
    entry.genericNameLower = StringUtils::toLower(entry.genericName);
    entry.keywordsLower = StringUtils::toLower(entry.keywords);
    entry.categoriesLower = StringUtils::toLower(entry.categories);
    entry.startupWmClassLower = StringUtils::toLower(entry.startupWmClass);
    entry.idLower = StringUtils::toLower(entry.id);
    entry.execLower = StringUtils::toLower(entry.exec);
    entry.origin = detectOrigin(
        filepath,
        hasAppImageMetadata || executableIsAppImage(entry.exec, isUserDesktopEntry(filepath, environment), environment),
        environment
    );

    // Build actions in the declared order.
    for (const auto& id : actionOrder) {
      auto it = actionMap.find(id);
      if (it != actionMap.end()) {
        entry.actions.push_back(
            DesktopAction{
                .id = it->first,
                .name = it->second.name,
                .exec = it->second.exec,
                .nameLower = StringUtils::toLower(it->second.name),
                .execLower = StringUtils::toLower(it->second.exec),
            }
        );
      }
    }

    entries.push_back(std::move(entry));
  }

  std::vector<std::string> xdgDataDirs(const DesktopEntryEnvironment& environment) {
    std::vector<std::string> dirs;
    std::unordered_set<std::string> seen;

    auto appendDir = [&](std::string dir) {
      if (dir.empty()) {
        return;
      }
      if (seen.insert(dir).second) {
        dirs.push_back(std::move(dir));
      }
    };

    if (!environment.dataHome.empty()) {
      appendDir(environment.dataHome);
    } else if (!environment.home.empty()) {
      appendDir(environment.home + "/.local/share");
    }

    if (!environment.dataDirs.empty()) {
      std::string_view sv(environment.dataDirs);
      std::size_t start = 0;
      while (start < sv.size()) {
        auto colon = sv.find(':', start);
        if (colon == std::string_view::npos) {
          appendDir(std::string(sv.substr(start)));
          break;
        }
        appendDir(std::string(sv.substr(start, colon - start)));
        start = colon + 1;
      }
    }

    // Keep canonical system directories as a safety net for partial env setups.
    appendDir("/usr/local/share");
    appendDir("/usr/share");

    return dirs;
  }

  std::vector<DesktopEntry> scanInEnvironment(const DesktopEntryEnvironment& environment, std::string_view language) {
    std::vector<DesktopEntry> entries;

    // Track seen IDs to deduplicate (first occurrence wins per XDG spec).
    // Hidden/NoDisplay files still claim their ID so user-local overrides can
    // suppress lower-priority system entries.
    std::unordered_set<std::string> seenIds;

    for (const auto& dataDir : xdgDataDirs(environment)) {
      fs::path appDir = fs::path(dataDir) / "applications";
      std::error_code ec;
      if (!fs::is_directory(appDir, ec)) {
        continue;
      }

      constexpr auto options = fs::directory_options::skip_permission_denied;
      for (fs::recursive_directory_iterator it(appDir, options, ec), end; it != end; it.increment(ec)) {
        if (ec) {
          ec.clear();
          continue;
        }
        if (!it->is_regular_file(ec)) {
          ec.clear();
          continue;
        }
        if (it->path().extension() != ".desktop") {
          continue;
        }

        std::string id = it->path().stem().string();
        if (!seenIds.insert(id).second) {
          continue;
        }

        parseDesktopFile(it->path(), language, environment, entries);
      }
    }

    // Sort by name for consistent ordering
    std::ranges::sort(entries, {}, &DesktopEntry::nameLower);

    return entries;
  }

} // namespace

noctalia::detail::DesktopEntryEnvironment noctalia::detail::DesktopEntryEnvironment::capture() {
  const auto copy = [](const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
  };
  return {
      .home = copy("HOME"),
      .path = copy("PATH"),
      .dataHome = copy("XDG_DATA_HOME"),
      .dataDirs = copy("XDG_DATA_DIRS"),
      .currentDesktop = copy("XDG_CURRENT_DESKTOP"),
  };
}

struct noctalia::detail::DesktopEntryCache::Impl {
  struct Request {
    DesktopEntryEnvironment environment;
    std::string language;
    std::uint64_t generation = 1;
    bool force = true;
  };
  struct Result {
    std::shared_ptr<const std::vector<DesktopEntry>> entries;
    std::uint64_t generation;
  };

  Impl(DesktopEntryEnvironment environment, std::string_view language, std::function<void()> beforeRefresh)
      : latest{std::move(environment), i18n::detail::normalizeLanguageTag(language)}, pending(latest),
        notifyFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)), beforeRefresh(std::move(beforeRefresh)),
        worker([this] { workerLoop(); }) {}

  ~Impl() {
    {
      std::scoped_lock lock(requestMutex);
      stopping = true;
    }
    wake.notify_one();
    // A filesystem syscall can block in the kernel. Join ensures the worker
    // never outlives its state; the service manager still bounds process stop.
    worker.join();
    if (notifyFd >= 0) {
      ::close(notifyFd);
    }
  }

  void requestRefresh(DesktopEntryEnvironment environment, bool force) {
    {
      std::scoped_lock lock(requestMutex);
      latest.environment = std::move(environment);
      queueLocked(force);
    }
    wake.notify_one();
  }

  void setLanguage(std::string_view language) {
    const auto normalized = i18n::detail::normalizeLanguageTag(language);
    {
      std::scoped_lock lock(requestMutex);
      if (latest.language == normalized) {
        return;
      }
      latest.language = normalized;
      queueLocked(true);
    }
    wake.notify_one();
  }

  void queueLocked(bool force) {
    ++latest.generation;
    latest.force = force || (pending.has_value() && pending->force);
    pending = latest; // replace, never append: one latest request is enough
  }

  static std::string sourceSignature(const DesktopEntryEnvironment& environment) {
    std::string signature = "xdg_current_desktop=" + environment.currentDesktop + '\n';
    for (const auto& dataDir : xdgDataDirs(environment)) {
      const fs::path appDir = fs::path(dataDir) / "applications";
      std::error_code ec;
      const fs::path resolved = fs::weakly_canonical(appDir, ec);
      const std::string path = ec ? appDir.string() : resolved.string();
      signature += path;
      struct ::stat st{};
      if (::stat(path.c_str(), &st) == 0) {
        signature += ':' + std::to_string(static_cast<unsigned long long>(st.st_dev));
        signature += ':' + std::to_string(static_cast<unsigned long long>(st.st_ino));
        signature += ':' + std::to_string(static_cast<long long>(st.st_mtim.tv_sec));
        signature += ':' + std::to_string(static_cast<long long>(st.st_mtim.tv_nsec));
      } else {
        signature += ":missing";
      }
      signature += '\n';
    }
    return signature;
  }

  static bool drainChanges(Inotify& inotify) {
    bool changed = false;
    inotify.drain([&changed](const inotify_event*) { changed = true; });
    return changed;
  }

  static void rebuildWatches(Inotify& inotify, std::vector<int>& watches, const DesktopEntryEnvironment& environment) {
    for (const auto wd : watches) {
      inotify.unwatch(wd);
    }
    watches.clear();
    if (inotify.fd() < 0) {
      return;
    }
    std::unordered_set<std::string> watchedPaths;
    const auto add = [&](const fs::path& path) {
      const auto key = path.string();
      if (!watchedPaths.insert(key).second) {
        return;
      }
      constexpr std::uint32_t mask = IN_CREATE
          | IN_DELETE
          | IN_MOVED_FROM
          | IN_MOVED_TO
          | IN_CLOSE_WRITE
          | IN_DELETE_SELF
          | IN_MOVE_SELF
          | IN_ATTRIB;
      if (const auto wd = inotify.watch(path, mask)) {
        watches.push_back(*wd);
      } else {
        kLog.warn("failed to watch desktop entry directory '{}'", key);
      }
    };
    for (const auto& dataDir : xdgDataDirs(environment)) {
      const fs::path appDir = fs::path(dataDir) / "applications";
      std::error_code ec;
      if (!fs::is_directory(appDir, ec)) {
        continue;
      }
      add(appDir);
      constexpr auto options = fs::directory_options::skip_permission_denied;
      for (fs::recursive_directory_iterator it(appDir, options, ec), end; it != end; it.increment(ec)) {
        if (ec) {
          ec.clear();
          continue;
        }
        if (it->is_directory(ec) && !ec) {
          add(it->path());
        }
        ec.clear();
      }
    }
  }

  void notify() const {
    if (notifyFd < 0) {
      return;
    }
    const std::uint64_t value = 1;
    while (::write(notifyFd, &value, sizeof(value)) < 0 && errno == EINTR) {
    }
  }

  void workerLoop() {
    // Inotify creation, watch mutations and event draining all stay here. The
    // main poll source watches only our completion eventfd.
    Inotify inotify;
    std::vector<int> watches;
    std::shared_ptr<const std::vector<DesktopEntry>> scanned;
    DesktopEntryEnvironment scannedEnvironment;
    std::string scannedLanguage;
    std::string signature;
    bool needsScan = true;
    for (;;) {
      std::optional<Request> request;
      {
        std::unique_lock lock(requestMutex);
        wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopping || pending.has_value(); });
        if (stopping) {
          return;
        }
        request = std::move(pending);
        pending.reset();
      }
      try {
        if (!request) {
          if (drainChanges(inotify)) {
            std::scoped_lock lock(requestMutex);
            queueLocked(true);
          }
          continue;
        }
        if (beforeRefresh) {
          beforeRefresh();
        }
        // Never hold requestMutex or entriesMutex across filesystem work.
        const auto before = sourceSignature(request->environment);
        const bool changed = drainChanges(inotify);
        if (needsScan
            || request->force
            || changed
            || before != signature
            || request->environment != scannedEnvironment
            || request->language != scannedLanguage) {
          rebuildWatches(inotify, watches, request->environment);
          // Discard notifications caused by removing/replacing old watches.
          // New watches are installed before the scan to catch in-place edits.
          (void)drainChanges(inotify);
          auto entries = std::make_shared<const std::vector<DesktopEntry>>(
              scanInEnvironment(request->environment, request->language)
          );
          const auto after = sourceSignature(request->environment);
          if (before != after || drainChanges(inotify)) {
            std::scoped_lock lock(requestMutex);
            queueLocked(true);
            continue; // an application source changed during the scan
          }
          scanned = std::move(entries);
          scannedEnvironment = request->environment;
          scannedLanguage = request->language;
          signature = after;
          needsScan = false;
        }
        {
          std::scoped_lock lock(requestMutex);
          if (stopping || request->generation != latest.generation) {
            continue;
          }
          completed = Result{scanned, request->generation};
        }
        notify();
      } catch (const std::exception& error) {
        needsScan = true;
        try {
          kLog.warn("desktop entry refresh failed; retaining last catalog: {}", error.what());
        } catch (...) {
          // Diagnostics must not turn a recoverable allocation failure fatal.
        }
      } catch (...) {
        needsScan = true;
        try {
          kLog.warn("desktop entry refresh failed; retaining last catalog");
        } catch (...) {
        }
      }
      // Failed scans retry on the next request/event, rather than spinning.
    }
  }

  void adopt() {
    if (notifyFd >= 0) {
      std::uint64_t value;
      while (::read(notifyFd, &value, sizeof(value)) > 0 || errno == EINTR) {
      }
    }
    {
      // Keep generation validation and publication one short transaction. A
      // worker watch event cannot invalidate the result between these steps.
      std::scoped_lock requestLock(requestMutex);
      if (!completed || completed->generation != latest.generation) {
        completed.reset();
        return;
      }
      auto result = std::move(*completed);
      completed.reset();
      std::scoped_lock entriesLock(entriesMutex);
      if (published == result.entries) {
        return;
      }
      published = std::move(result.entries);
      ++version;
    }
    // Subscribers may request another refresh; no cache mutex is held here.
    changed.emit();
  }

  mutable std::mutex entriesMutex;
  std::shared_ptr<const std::vector<DesktopEntry>> published = std::make_shared<const std::vector<DesktopEntry>>();
  std::uint64_t version = 0; // main thread only, like published reference getters
  Signal<> changed;          // main thread only
  std::mutex requestMutex;
  std::condition_variable wake;
  Request latest;
  std::optional<Request> pending;
  std::optional<Result> completed;
  bool stopping = false;
  int notifyFd;
  std::function<void()> beforeRefresh;
  std::thread worker;
};

namespace noctalia::detail {
  DesktopEntryCache::DesktopEntryCache(
      DesktopEntryEnvironment environment, std::string_view language, std::function<void()> beforeRefresh
  )
      : m_impl(std::make_unique<Impl>(std::move(environment), language, std::move(beforeRefresh))) {}
  DesktopEntryCache::~DesktopEntryCache() = default;
  const std::vector<DesktopEntry>& DesktopEntryCache::entries() const { return *m_impl->published; }
  std::shared_ptr<const std::vector<DesktopEntry>> DesktopEntryCache::entriesSnapshot() const {
    std::scoped_lock lock(m_impl->entriesMutex);
    return m_impl->published;
  }
  std::uint64_t DesktopEntryCache::version() const { return m_impl->version; }
  int DesktopEntryCache::watchFd() const noexcept { return m_impl->notifyFd; }
  void DesktopEntryCache::checkReload() { m_impl->adopt(); }
  void DesktopEntryCache::checkSourcesChanged(DesktopEntryEnvironment environment) {
    m_impl->requestRefresh(std::move(environment), false);
  }
  void DesktopEntryCache::setLanguage(std::string_view language) { m_impl->setLanguage(language); }
  Signal<>& DesktopEntryCache::changed() { return m_impl->changed; }
} // namespace noctalia::detail

namespace {
  noctalia::detail::DesktopEntryCache& cache() {
    static noctalia::detail::DesktopEntryCache instance(DesktopEntryEnvironment::capture());
    return instance;
  }
} // namespace

std::vector<DesktopEntry> scanDesktopEntries(std::string_view language) {
  return scanInEnvironment(DesktopEntryEnvironment::capture(), language);
}
const std::vector<DesktopEntry>& desktopEntries() { return cache().entries(); }
std::shared_ptr<const std::vector<DesktopEntry>> desktopEntriesSnapshot() { return cache().entriesSnapshot(); }
std::uint64_t desktopEntriesVersion() { return cache().version(); }
void setDesktopEntryLanguage(std::string_view language) {
  cache().checkSourcesChanged(DesktopEntryEnvironment::capture());
  cache().setLanguage(language);
}
int desktopEntryWatchFd() noexcept { return cache().watchFd(); }
void checkDesktopEntryReload() { cache().checkReload(); }
Signal<>& desktopEntriesChanged() { return cache().changed(); }
void refreshDesktopEntriesIfSourcesChanged() { cache().checkSourcesChanged(DesktopEntryEnvironment::capture()); }
