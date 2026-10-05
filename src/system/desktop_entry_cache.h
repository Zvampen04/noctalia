#pragma once

#include "system/desktop_entry.h"

#include <functional>
#include <memory>
#include <string>

namespace noctalia::detail {

  // Copy process configuration before a refresh is queued. The worker never
  // reads mutable getenv storage while discovering or parsing applications.
  struct DesktopEntryEnvironment {
    std::string home;
    std::string path;
    std::string dataHome;
    std::string dataDirs;
    std::string currentDesktop;

    static DesktopEntryEnvironment capture();
    bool operator==(const DesktopEntryEnvironment&) const = default;
  };

  // Private catalog implementation, also instantiated by native lifecycle
  // tests. beforeRefresh can gate that instance's real worker I/O; production
  // leaves it empty. No test hook changes the process-global catalog.
  class DesktopEntryCache {
  public:
    explicit DesktopEntryCache(
        DesktopEntryEnvironment environment, std::string_view language = {}, std::function<void()> beforeRefresh = {}
    );
    ~DesktopEntryCache();
    DesktopEntryCache(const DesktopEntryCache&) = delete;
    DesktopEntryCache& operator=(const DesktopEntryCache&) = delete;

    const std::vector<DesktopEntry>& entries() const;
    std::shared_ptr<const std::vector<DesktopEntry>> entriesSnapshot() const;
    std::uint64_t version() const;
    int watchFd() const noexcept;
    void checkReload();
    void checkSourcesChanged(DesktopEntryEnvironment environment);
    void setLanguage(std::string_view language);
    Signal<>& changed();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace noctalia::detail
