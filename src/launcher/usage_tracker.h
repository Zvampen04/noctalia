#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// Main-thread usage/ranking state. One owned worker reads the initial history
// and coalesces saves into one active and one latest pending immutable snapshot.
class UsageTracker {
public:
  struct IoHooks {
    // Private-instance native tests may gate the real read/write. Production
    // leaves these empty; no process-global or user-state test hook is installed.
    std::function<void()> beforeLoad;
    std::function<void()> beforeSave;
  };

  UsageTracker();
  explicit UsageTracker(std::string stateDirectory, IoHooks hooks = {});
  ~UsageTracker();
  UsageTracker(const UsageTracker&) = delete;
  UsageTracker& operator=(const UsageTracker&) = delete;

  void setLoadedCallback(std::function<void()> callback);
  [[nodiscard]] bool loaded() const;
  void record(std::string_view providerId, std::string_view resultId);
  void clear();
  [[nodiscard]] int getCount(std::string_view providerId, std::string_view resultId);
  [[nodiscard]] int getRecentlyUsedIndex(std::string_view providerId, std::string_view resultId);
  [[nodiscard]] std::size_t getRecentlyUsedCount(std::string_view providerId);

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
