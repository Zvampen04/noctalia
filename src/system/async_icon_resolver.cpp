#include "system/async_icon_resolver.h"

#include "core/deferred_call.h"
#include "core/log.h"
#include "system/icon_resolver.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <unistd.h>
#include <unordered_set>
#include <utility>

namespace {
  void warnLookupFailure(const std::string& name, const char* detail = nullptr) noexcept {
    try {
      if (detail != nullptr) {
        Logger("async-icon").warn("failed to resolve icon '{}': {}", name, detail);
      } else {
        Logger("async-icon").warn("failed to resolve icon '{}'", name);
      }
    } catch (...) {
      // Formatting/logging may itself allocate during allocation pressure.
    }
  }

  void warnPublicationFailure() noexcept {
    constexpr char message[] = "noctalia async-icon: cannot publish metadata; later binding may retry\n";
    (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
  }
} // namespace

struct AsyncIconResolver::State {
  struct Request {
    std::string key;
    std::string name;
    int size = 0;
    std::uint64_t generation = 0;
  };
  struct Result {
    std::string key;
    std::string path;
    std::uint64_t generation = 0;
    bool retrySoon = false;
  };
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<Request> requests;
  std::deque<Result> results;
  // Current-generation queued, executing and completed requests until adoption.
  // An invalidated in-flight lookup finishes separately and cannot publish.
  std::unordered_set<std::string> pending;
  std::uint64_t generation = 1;
  bool notificationPending = false;
  bool stopping = false;
};

AsyncIconResolver::AsyncIconResolver(ResolveFunction resolve) : m_state(std::make_shared<State>()) {
  const std::weak_ptr<int> alive = m_alive;
  m_worker = std::thread([state = m_state, resolve = std::move(resolve), this, alive]() mutable {
    workerLoop(state, std::move(resolve), this, alive);
  });
}

AsyncIconResolver::~AsyncIconResolver() {
  m_alive.reset();
  {
    std::scoped_lock lock(m_state->mutex);
    m_state->stopping = true;
    m_state->requests.clear();
    m_state->results.clear();
    m_state->pending.clear();
  }
  m_state->cv.notify_one();
  // No detached work or callbacks outlive this object. A currently executing
  // filesystem syscall is not cancellable, so shutdown waits for that lookup.
  m_worker.join();
}

std::string AsyncIconResolver::makeKey(const std::string& name, int size) {
  return name + '\x1F' + std::to_string(std::max(0, size));
}

std::string AsyncIconResolver::resolveOrRequest(const std::string& name, int size) {
  if (name.empty()) {
    return {};
  }
  const auto key = makeKey(name, size);
  if (const auto it = m_cache.find(key); it != m_cache.end()) {
    if (std::chrono::steady_clock::now() < it->second.retryAfter) {
      it->second.touch = ++m_touch;
      return it->second.path;
    }
    m_cache.erase(it);
  }
  {
    std::scoped_lock lock(m_state->mutex);
    if (m_state->stopping || m_state->pending.contains(key)) {
      return {};
    }
    if (m_state->pending.size() >= kMaxPending) {
      if (m_state->requests.empty()) {
        return {};
      }
      // Rapid scrolling favours the newest visible demand over stale queued rows.
      m_state->pending.erase(m_state->requests.front().key);
      m_state->requests.pop_front();
    }
    m_state->pending.insert(key);
    m_state->requests.push_back({key, name, std::max(0, size), m_state->generation});
  }
  m_state->cv.notify_one();
  return {};
}

void AsyncIconResolver::setReadyCallback(std::function<void()> callback) { m_readyCallback = std::move(callback); }

void AsyncIconResolver::advanceGeneration() {
  std::scoped_lock lock(m_state->mutex);
  ++m_state->generation;
  m_state->requests.clear();
  m_state->results.clear();
  m_state->pending.clear();
}

void AsyncIconResolver::invalidate() {
  advanceGeneration();
  m_cache.clear();
}

void AsyncIconResolver::cancelPending() { advanceGeneration(); }

void AsyncIconResolver::drainResults() {
  std::deque<State::Result> results;
  {
    std::scoped_lock lock(m_state->mutex);
    m_state->notificationPending = false;
    results.swap(m_state->results);
    for (const auto& result : results) {
      m_state->pending.erase(result.key);
    }
  }
  for (auto& result : results) {
    if (!m_cache.contains(result.key) && m_cache.size() >= kMaxCacheEntries) {
      const auto oldest = std::ranges::min_element(m_cache, {}, [](const auto& entry) { return entry.second.touch; });
      m_cache.erase(oldest);
    }
    const auto retryAfter = result.retrySoon ? std::chrono::steady_clock::now() + std::chrono::seconds{1}
                                             : std::chrono::steady_clock::time_point::max();
    m_cache.insert_or_assign(std::move(result.key), CacheEntry{std::move(result.path), ++m_touch, retryAfter});
  }
  if (!results.empty() && m_readyCallback) {
    m_readyCallback();
  }
}

void AsyncIconResolver::workerLoop(
    const std::shared_ptr<State>& state, ResolveFunction resolve, AsyncIconResolver* owner, std::weak_ptr<int> alive
) {
  std::unique_ptr<IconResolver> resolver;
  std::uint64_t resolverGeneration = 0;
  for (;;) {
    State::Request request;
    {
      std::unique_lock lock(state->mutex);
      state->cv.wait(lock, [&]() { return state->stopping || !state->requests.empty(); });
      if (state->stopping) {
        return;
      }
      request = std::move(state->requests.front());
      state->requests.pop_front();
    }
    std::string path;
    bool retrySoon = false;
    try {
      if (resolve) {
        path = resolve(request.name, request.size);
        retrySoon = path.empty() && request.name.front() == '/';
      } else {
        if (resolverGeneration != request.generation) {
          resolver = std::make_unique<IconResolver>(true, kMaxCacheEntries);
          resolverGeneration = request.generation;
        }
        path = resolver->resolve(request.name, request.size);
        retrySoon = path.empty() && request.name.front() == '/';
        if (path.empty() && request.name != "application-x-executable") {
          path = resolver->resolve("application-x-executable", request.size);
        }
      }
    } catch (const std::exception& error) {
      retrySoon = true;
      warnLookupFailure(request.name, error.what());
    } catch (...) {
      retrySoon = true;
      warnLookupFailure(request.name);
    }
    bool signal = false;
    try {
      {
        std::scoped_lock lock(state->mutex);
        if (state->stopping) {
          return;
        }
        if (request.generation != state->generation) {
          continue;
        }
        // Keep the request key until insertion succeeds, so allocation failure
        // can release its pending marker without touching another generation.
        state->results.push_back({request.key, std::move(path), request.generation, retrySoon});
        if (!state->notificationPending) {
          state->notificationPending = true;
          signal = true;
        }
      }
    } catch (...) {
      {
        std::scoped_lock lock(state->mutex);
        if (request.generation == state->generation) {
          state->pending.erase(request.key);
        }
      }
      warnPublicationFailure();
      continue;
    }
    if (signal) {
      try {
        DeferredCall::callLater([owner, alive]() {
          if (!alive.expired()) {
            owner->drainResults();
          }
        });
      } catch (...) {
        {
          std::scoped_lock lock(state->mutex);
          // No callback can deliver these completed results. Release them for a
          // later visible bind; queued requests remain bounded and can progress.
          for (const auto& result : state->results) {
            state->pending.erase(result.key);
          }
          state->results.clear();
          state->notificationPending = false;
        }
        warnPublicationFailure();
      }
    }
  }
}
