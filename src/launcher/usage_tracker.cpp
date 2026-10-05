#include "launcher/usage_tracker.h"

#include "config/atomic_file.h"
#include "core/deferred_call.h"
#include "core/log.h"
#include "util/file_utils.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {
  constexpr Logger kLog("launcher-usage");
  constexpr std::size_t kMaxRecentlyUsedCount = 20;

  void warnIoFailure(std::string_view message, const char* detail = nullptr) noexcept {
    try {
      if (detail != nullptr) {
        kLog.warn("{}: {}", message, detail);
      } else {
        kLog.warn("{}", message);
      }
    } catch (...) {
      // A second allocation failure in diagnostics must not escape the worker.
    }
  }

  struct History {
    std::unordered_map<std::string, std::unordered_map<std::string, int>> counts;
    std::unordered_map<std::string, std::deque<std::string>> recent;
  };

  void remember(std::deque<std::string>& recent, const std::string& id) {
    std::erase(recent, id);
    recent.push_front(id);
    while (recent.size() > kMaxRecentlyUsedCount) {
      recent.pop_back();
    }
  }

  History mergeInitial(const History& baseline, const History& delta) {
    History merged = baseline;
    for (const auto& [provider, entries] : delta.counts) {
      for (const auto& [id, count] : entries) {
        auto& total = merged.counts[provider][id];
        total = static_cast<int>(
            std::min<long long>(static_cast<long long>(total) + count, std::numeric_limits<int>::max())
        );
      }
    }
    for (const auto& [provider, entries] : delta.recent) {
      auto& recent = merged.recent[provider];
      for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        remember(recent, *it);
      }
    }
    return merged;
  }

  std::optional<nlohmann::json> readJson(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
      std::error_code error;
      if (std::filesystem::exists(path, error) || error) {
        throw std::runtime_error("cannot read existing usage history: " + path);
      }
      return std::nullopt; // Genuinely absent history is a valid empty baseline.
    }
    try {
      auto result = nlohmann::json::parse(file);
      if (file.bad()) {
        throw std::runtime_error("usage history read failed: " + path);
      }
      return result;
    } catch (const nlohmann::json::exception&) {
      if (file.bad()) {
        throw std::runtime_error("usage history read failed: " + path);
      }
      return std::nullopt; // Preserve the existing ignore-malformed-file policy.
    }
  }

  History readHistory(const std::string& countsPath, const std::string& recentPath) {
    History history;
    if (const auto json = readJson(countsPath)) {
      try {
        for (const auto& [provider, ids] : json->items()) {
          for (const auto& [id, count] : ids.items()) {
            history.counts[provider][id] = count.get<int>();
          }
        }
      } catch (const nlohmann::json::exception&) {
      }
    }
    if (const auto json = readJson(recentPath)) {
      try {
        for (const auto& [provider, ids] : json->items()) {
          auto& recent = history.recent[provider];
          for (const auto& value : ids) {
            const auto id = value.get<std::string>();
            if (recent.size() < kMaxRecentlyUsedCount && !std::ranges::contains(recent, id)) {
              recent.push_back(id);
            }
          }
        }
      } catch (const nlohmann::json::exception&) {
      }
    }
    return history;
  }
} // namespace

struct UsageTracker::Impl {
  struct Save {
    History history;
    bool mergeOriginal = false;
    std::uint64_t revision = 0;
  };
  struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::shared_ptr<const Save> latest;
    std::shared_ptr<const History> initialResult;
    bool stopping = false;
  };

  Impl(std::string directory, IoHooks hooks)
      : countsPath((directory.empty() ? "." : directory) + "/usage_counts.json"),
        recentPath((directory.empty() ? "." : directory) + "/recently_used.json"), hooks(std::move(hooks)),
        worker([this] { workerLoop(); }) {}

  ~Impl() {
    alive.reset();
    loadedCallback = {};
    {
      std::scoped_lock lock(state->mutex);
      state->stopping = true;
    }
    state->wake.notify_one();
    // Flush the latest accepted revision before joining. Kernel IO may block;
    // failed IO is reported, not converted into a successful persistence claim.
    worker.join();
  }

  void queueSave() {
    auto snapshot = std::make_shared<const Save>(Save{history, !isLoaded, ++revision});
    {
      std::scoped_lock lock(state->mutex);
      state->latest = std::move(snapshot);
    }
    state->wake.notify_one();
  }

  void adoptInitial() {
    std::shared_ptr<const History> initial;
    {
      std::scoped_lock lock(state->mutex);
      initial = std::move(state->initialResult);
    }
    if (!initial || isLoaded) {
      return; // A clear already made the cached state authoritative.
    }
    history = mergeInitial(*initial, history);
    isLoaded = true;
    // Already queued pre-adoption saves merge these same deltas against the
    // original worker baseline. Adoption itself requires no redundant write.
    if (loadedCallback) {
      loadedCallback();
    }
  }

  std::shared_ptr<const History> loadInitial() {
    try {
      if (hooks.beforeLoad) {
        hooks.beforeLoad();
      }
      auto initial = std::make_shared<const History>(readHistory(countsPath, recentPath));
      {
        std::scoped_lock lock(state->mutex);
        state->initialResult = initial;
      }
      DeferredCall::callLater([this, lifetime = lifetime] {
        if (!lifetime.expired()) {
          adoptInitial();
        }
      });
      return initial;
    } catch (const std::exception& error) {
      warnIoFailure("cannot load usage history; retaining existing files", error.what());
    } catch (...) {
      warnIoFailure("cannot load usage history; retaining existing files");
    }
    return {};
  }

  void workerLoop() {
    // Keep this ORIGINAL baseline immutable across pre-adoption saves. Rereading
    // a file already containing deltas would count those activations twice.
    auto original = loadInitial();
    std::uint64_t attempted = 0;
    std::uint64_t saved = 0;
    for (;;) {
      std::shared_ptr<const Save> request;
      bool stopping;
      {
        std::unique_lock lock(state->mutex);
        state->wake.wait(lock, [&] {
          return state->stopping || (state->latest && state->latest->revision != attempted);
        });
        stopping = state->stopping;
        if (stopping && (!state->latest || state->latest->revision == saved)) {
          return;
        }
        request = state->latest;
        attempted = request->revision;
      }
      try {
        if (request->mergeOriginal && !original) {
          original = loadInitial();
        }
        if (request->mergeOriginal && !original) {
          throw std::runtime_error("original history remains unreadable; new usage is not persisted");
        }
        const auto snapshot = request->mergeOriginal ? mergeInitial(*original, request->history) : request->history;
        if (hooks.beforeSave) {
          hooks.beforeSave();
        }
        const auto counts = nlohmann::json(snapshot.counts).dump(2) + '\n';
        const auto recent = nlohmann::json(snapshot.recent).dump(2) + '\n';
        if (!writeTextFileAtomic(countsPath, counts, FileUtils::privateFileMode())
            || !writeTextFileAtomic(recentPath, recent, FileUtils::privateFileMode())) {
          throw std::runtime_error("atomic usage-history write failed");
        }
        saved = request->revision;
      } catch (const std::exception& error) {
        warnIoFailure("usage history save failed", error.what());
      } catch (...) {
        warnIoFailure("usage history save failed");
      }
      if (stopping) {
        return; // One final flush attempt; never spin on persistent IO failure.
      }
    }
  }

  std::string countsPath;
  std::string recentPath;
  IoHooks hooks;
  History history; // authoritative after adoption/clear; aggregated deltas before
  bool isLoaded = false;
  std::uint64_t revision = 0;
  std::function<void()> loadedCallback;
  std::shared_ptr<int> alive = std::make_shared<int>(0);
  const std::weak_ptr<int> lifetime = alive;
  std::shared_ptr<State> state = std::make_shared<State>();
  std::thread worker;
};

UsageTracker::UsageTracker() : UsageTracker(FileUtils::stateDir()) {}
UsageTracker::UsageTracker(std::string stateDirectory, IoHooks hooks)
    : m_impl(std::make_unique<Impl>(std::move(stateDirectory), std::move(hooks))) {}
UsageTracker::~UsageTracker() = default;
void UsageTracker::setLoadedCallback(std::function<void()> callback) { m_impl->loadedCallback = std::move(callback); }
bool UsageTracker::loaded() const { return m_impl->isLoaded; }

void UsageTracker::record(std::string_view providerId, std::string_view resultId) {
  auto& count = m_impl->history.counts[std::string(providerId)][std::string(resultId)];
  if (count < std::numeric_limits<int>::max()) {
    ++count;
  }
  remember(m_impl->history.recent[std::string(providerId)], std::string(resultId));
  m_impl->queueSave();
}

void UsageTracker::clear() {
  m_impl->history = {};
  m_impl->isLoaded = true;
  m_impl->queueSave();
}

int UsageTracker::getCount(std::string_view providerId, std::string_view resultId) {
  const auto provider = m_impl->history.counts.find(std::string(providerId));
  if (provider == m_impl->history.counts.end()) {
    return 0;
  }
  const auto entry = provider->second.find(std::string(resultId));
  return entry == provider->second.end() ? 0 : entry->second;
}

int UsageTracker::getRecentlyUsedIndex(std::string_view providerId, std::string_view resultId) {
  const auto provider = m_impl->history.recent.find(std::string(providerId));
  if (provider == m_impl->history.recent.end()) {
    return 0;
  }
  const auto& entries = provider->second;
  const auto entry = std::ranges::find(entries, resultId);
  return entry == entries.end() ? 0 : static_cast<int>(entries.end() - entry);
}

std::size_t UsageTracker::getRecentlyUsedCount(std::string_view providerId) {
  const auto provider = m_impl->history.recent.find(std::string(providerId));
  return provider == m_impl->history.recent.end() ? 0 : provider->second.size();
}
