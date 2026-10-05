#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

// Main-thread facade for filesystem icon lookup. A single owned worker resolves
// visible tiles; metadata lookup never runs in resolveOrRequest() or its callback.
class AsyncIconResolver {
public:
  using ResolveFunction = std::function<std::string(const std::string&, int)>;
  // Current-generation queued/executing/completed jobs. Cancellation may leave
  // one obsolete lookup executing on the sole worker until its syscall returns.
  static constexpr std::size_t kMaxPending = 128;
  static constexpr std::size_t kMaxCacheEntries = 512;

  explicit AsyncIconResolver(ResolveFunction resolve = {});
  ~AsyncIconResolver();
  AsyncIconResolver(const AsyncIconResolver&) = delete;
  AsyncIconResolver& operator=(const AsyncIconResolver&) = delete;

  [[nodiscard]] std::string resolveOrRequest(const std::string& name, int size);
  void setReadyCallback(std::function<void()> callback);
  // Theme/catalog changes invalidate positive and negative metadata together.
  void invalidate();
  // Closing a view cancels adoption and pending work, retaining bounded cache.
  void cancelPending();

private:
  struct State;
  struct CacheEntry {
    std::string path;
    std::uint64_t touch = 0;
    std::chrono::steady_clock::time_point retryAfter = std::chrono::steady_clock::time_point::max();
  };

  void drainResults();
  void advanceGeneration();
  static void workerLoop(
      const std::shared_ptr<State>& state, ResolveFunction resolve, AsyncIconResolver* owner, std::weak_ptr<int> alive
  );
  static std::string makeKey(const std::string& name, int size);

  std::shared_ptr<State> m_state;
  std::thread m_worker;
  std::shared_ptr<int> m_alive = std::make_shared<int>(0);
  std::unordered_map<std::string, CacheEntry> m_cache;
  std::uint64_t m_touch = 0;
  std::function<void()> m_readyCallback;
};
