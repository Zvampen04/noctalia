#include "system/async_icon_resolver.h"
#include "system/icon_resolver.h"
#include "system/icon_theme_poll_source.h"
#include "tests/async_metadata_test_support.h"

#include <atomic>
#include <dlfcn.h>
#include <fstream>
#include <gio/gio.h>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>

namespace allocation_probe {
  thread_local bool failNext = false;
  std::atomic<int> failures{0};
} // namespace allocation_probe

void* operator new(std::size_t size) {
  if (std::exchange(allocation_probe::failNext, false)) {
    ++allocation_probe::failures;
    throw std::bad_alloc();
  }
  if (void* value = std::malloc(std::max<std::size_t>(size, 1))) {
    return value;
  }
  throw std::bad_alloc();
}
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace settings_probe {
  struct Probe;
  struct Record {
    Probe* probe;
    std::uintptr_t identity;
    std::thread::id owner;
    std::uintptr_t context;
    std::vector<std::string> reads;
    bool finalized = false;
  };
  struct Probe {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::unique_ptr<Record>> records;
    bool releaseFirst = false;
    std::function<void()> afterRead;
  };
  thread_local Probe* active = nullptr;
  thread_local bool holdFirst = false;
  thread_local Probe* invokeProbe = nullptr;
  thread_local noctalia::test::metadata::IoGate* invokeGate = nullptr;

  void finalized(gpointer data, GObject*) {
    auto& record = *static_cast<Record*>(data);
    std::scoped_lock lock(record.probe->mutex);
    TEST_CHECK(std::this_thread::get_id() == record.owner);
    TEST_CHECK(!record.finalized);
    record.finalized = true;
    record.probe->wake.notify_all();
  }
} // namespace settings_probe

// Instrument actual GLib calls in this test binary only. Opaque identities are
// compared across records; no thread uses another thread's GSettings object or
// context. Creation, reads and weak-finalization all occur on the owning thread.
extern "C" GSettings* g_settings_new(const gchar* schema) {
  using Create = GSettings* (*)(const gchar*);
  static const auto real = reinterpret_cast<Create>(::dlsym(RTLD_NEXT, "g_settings_new"));
  TEST_CHECK(real != nullptr);
  GSettings* settings = real(schema);
  if (auto* probe = settings_probe::active) {
    GMainContext* context = g_main_context_get_thread_default();
    TEST_CHECK(context != nullptr && g_main_context_is_owner(context));
    std::unique_lock lock(probe->mutex);
    auto record = std::make_unique<settings_probe::Record>(settings_probe::Record{
        probe,
        reinterpret_cast<std::uintptr_t>(settings),
        std::this_thread::get_id(),
        reinterpret_cast<std::uintptr_t>(context),
        {},
        false
    });
    g_object_weak_ref(G_OBJECT(settings), settings_probe::finalized, record.get());
    probe->records.push_back(std::move(record));
    probe->wake.notify_all();
    if (std::exchange(settings_probe::holdFirst, false)) {
      TEST_CHECK(probe->wake.wait_for(lock, std::chrono::seconds(5), [&] { return probe->releaseFirst; }));
    }
  }
  return settings;
}

extern "C" gchar* g_settings_get_string(GSettings* settings, const gchar* key) {
  using Read = gchar* (*)(GSettings*, const gchar*);
  static const auto real = reinterpret_cast<Read>(::dlsym(RTLD_NEXT, "g_settings_get_string"));
  TEST_CHECK(real != nullptr);
  gchar* value = real(settings, key);
  if (auto* probe = settings_probe::active; probe && std::string_view(key) == "icon-theme") {
    {
      std::scoped_lock lock(probe->mutex);
      bool found = false;
      for (auto it = probe->records.rbegin(); it != probe->records.rend(); ++it) {
        auto& record = **it;
        if (record.identity == reinterpret_cast<std::uintptr_t>(settings) && !record.finalized) {
          TEST_CHECK(record.owner == std::this_thread::get_id());
          record.reads.emplace_back(value ? value : "");
          found = true;
          break;
        }
      }
      TEST_CHECK(found);
    }
    if (probe->afterRead) {
      probe->afterRead();
    }
  }
  return value;
}

extern "C" void g_main_context_invoke(GMainContext* context, GSourceFunc function, gpointer data) {
  using Invoke = void (*)(GMainContext*, GSourceFunc, gpointer);
  static const auto real = reinterpret_cast<Invoke>(::dlsym(RTLD_NEXT, "g_main_context_invoke"));
  TEST_CHECK(real != nullptr);
  if (auto* probe = settings_probe::invokeProbe) {
    bool target = false;
    {
      std::scoped_lock lock(probe->mutex);
      target = std::ranges::any_of(probe->records, [context](const auto& record) {
        return !record->finalized && record->context == reinterpret_cast<std::uintptr_t>(context);
      });
    }
    if (target) {
      TEST_CHECK(settings_probe::invokeGate != nullptr);
      settings_probe::invokeGate->enter();
    }
  }
  real(context, function, data);
}

namespace {

  using namespace noctalia::test::metadata;

  void coldLookupLeavesStatusAndHeartbeatResponsive() {
    TemporaryDirectory directory;
    const auto theme = directory.path / "icons/hicolor";
    const auto icon = theme / "scalable/apps/cold-probe.svg";
    std::filesystem::create_directories(icon.parent_path());
    std::ofstream(
        theme / "index.theme"
    ) << "[Icon Theme]\nDirectories=scalable/apps\n[scalable/apps]\nSize=64\nType=Scalable\n";
    std::ofstream(icon) << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\"/>";
    ScopedEnvironment home("HOME", directory.path.string());
    ScopedEnvironment dataHome("XDG_DATA_HOME", directory.path.string());
    ScopedEnvironment dataDirs("XDG_DATA_DIRS", directory.path.string());
    ResponsiveLoop loop;
    IoGate io;
    const auto mainThread = std::this_thread::get_id();
    std::atomic<int> calls{0};
    int notifications = 0;
    AsyncIconResolver resolver([&](const std::string& name, int size) {
      ++calls;
      io.enter();
      // First construction and the real cold filesystem lookup occur on the
      // worker. The fixture does not warm the resolver before opening a view.
      IconResolver actual(true);
      return std::string(actual.resolve(name, size));
    });
    resolver.setReadyCallback([&] {
      TEST_CHECK(std::this_thread::get_id() == mainThread);
      ++notifications;
    });
    const auto started = Clock::now();
    TEST_CHECK(resolver.resolveOrRequest("cold-probe", 32).empty());
    TEST_CHECK(Clock::now() - started < 100ms);
    loop.until([&] { return io.started(); });
    TEST_CHECK(io.worker() != mainThread);
    const auto beatsBefore = loop.beats();
    loop.checkStatus();
    loop.pump(200ms);
    loop.checkStatus();
    TEST_CHECK(loop.beats() >= beatsBefore + 15);
    TEST_CHECK(loop.maxHeartbeatGap() < 100ms);
    TEST_CHECK(calls == 1);
    TEST_CHECK(notifications == 0);
    io.release();
    loop.until([&] { return notifications != 0; });
    TEST_CHECK(resolver.resolveOrRequest("cold-probe", 32) == icon.string());
    TEST_CHECK(calls == 1);
  }

  void coalescesAndBoundsBurstWithoutLosingLatestVisibleDemand() {
    ResponsiveLoop loop;
    IoGate io;
    std::atomic<std::size_t> calls{0};
    AsyncIconResolver resolver([&](const std::string& name, int) {
      if (++calls == 1) {
        io.enter();
      }
      return "/private-icons/" + name;
    });
    TEST_CHECK(resolver.resolveOrRequest("first", 32).empty());
    loop.until([&] { return io.started(); });
    for (int i = 0; i < 256; ++i) {
      TEST_CHECK(resolver.resolveOrRequest("first", 32).empty());
    }
    constexpr std::size_t burst = AsyncIconResolver::kMaxPending * 8;
    for (std::size_t i = 0; i < burst; ++i) {
      TEST_CHECK(resolver.resolveOrRequest("burst-" + std::to_string(i), 32).empty());
    }
    TEST_CHECK(calls == 1);
    loop.checkStatus();
    io.release();
    loop.until([&] { return calls == AsyncIconResolver::kMaxPending; });
    loop.pump(20ms);
    TEST_CHECK(calls == AsyncIconResolver::kMaxPending);
    const auto latest = "burst-" + std::to_string(burst - 1);
    TEST_CHECK(resolver.resolveOrRequest(latest, 32) == "/private-icons/" + latest);
    TEST_CHECK(resolver.resolveOrRequest("first", 32) == "/private-icons/first");
    const auto before = calls.load();
    TEST_CHECK(resolver.resolveOrRequest("burst-0", 32).empty());
    loop.until([&] { return resolver.resolveOrRequest("burst-0", 32) == "/private-icons/burst-0"; });
    TEST_CHECK(calls == before + 1);
  }

  void invalidationRejectsOldGenerationAndCancelPreservesCompletedCache() {
    ResponsiveLoop loop;
    IoGate io;
    std::atomic<int> calls{0};
    AsyncIconResolver resolver([&](const std::string&, int) {
      const int call = ++calls;
      if (call == 1) {
        io.enter();
      }
      return call == 1 ? "/old-generation" : "/new-generation";
    });
    TEST_CHECK(resolver.resolveOrRequest("same", 32).empty());
    loop.until([&] { return io.started(); });
    resolver.invalidate();
    TEST_CHECK(resolver.resolveOrRequest("same", 32).empty());
    for (std::size_t i = 1; i < AsyncIconResolver::kMaxPending; ++i) {
      TEST_CHECK(resolver.resolveOrRequest("new-generation-" + std::to_string(i), 32).empty());
    }
    io.release();
    loop.until([&] { return resolver.resolveOrRequest("same", 32) == "/new-generation"; });
    loop.until([&] { return calls == static_cast<int>(AsyncIconResolver::kMaxPending + 1); });
    // One obsolete syscall can still be executing when a generation changes;
    // the 128 current requests remain bounded independently of that old job.
    TEST_CHECK(calls == static_cast<int>(AsyncIconResolver::kMaxPending + 1));
    resolver.cancelPending();
    TEST_CHECK(resolver.resolveOrRequest("same", 32) == "/new-generation");
    TEST_CHECK(calls == static_cast<int>(AsyncIconResolver::kMaxPending + 1));

    IoGate cancelled;
    std::atomic<int> cancelledCalls{0};
    int callbacks = 0;
    AsyncIconResolver closedView([&](const std::string& name, int) {
      if (++cancelledCalls == 1) {
        cancelled.enter();
      }
      return "/icons/" + name;
    });
    closedView.setReadyCallback([&] { ++callbacks; });
    TEST_CHECK(closedView.resolveOrRequest("executing", 32).empty());
    loop.until([&] { return cancelled.started(); });
    TEST_CHECK(closedView.resolveOrRequest("queued", 32).empty());
    closedView.cancelPending();
    cancelled.release();
    loop.pump(40ms);
    TEST_CHECK(cancelledCalls == 1);
    TEST_CHECK(callbacks == 0);
    TEST_CHECK(closedView.resolveOrRequest("fresh", 32).empty());
    loop.until([&] { return closedView.resolveOrRequest("fresh", 32) == "/icons/fresh"; });
    TEST_CHECK(callbacks > 0);
  }

  void cacheHasPositiveAndNegativeBoundsAndWorkerSurvivesFailedLookup() {
    ResponsiveLoop loop;
    std::atomic<std::size_t> calls{0};
    int callbacks = 0;
    AsyncIconResolver resolver([&](const std::string& name, int) -> std::string {
      ++calls;
      if (name == "broken") {
        throw std::runtime_error("controlled metadata failure");
      }
      return name == "missing" ? std::string{} : "/icons/" + name;
    });
    resolver.setReadyCallback([&] { ++callbacks; });
    TEST_CHECK(resolver.resolveOrRequest("broken", 32).empty());
    loop.until([&] { return callbacks != 0; });
    const auto afterFailure = calls.load();
    TEST_CHECK(resolver.resolveOrRequest("broken", 32).empty());
    loop.pump(20ms);
    TEST_CHECK(calls == afterFailure);
    TEST_CHECK(resolver.resolveOrRequest("good", 32).empty());
    loop.until([&] { return resolver.resolveOrRequest("good", 32) == "/icons/good"; });
    const auto previousCallbacks = callbacks;
    TEST_CHECK(resolver.resolveOrRequest("missing", 32).empty());
    loop.until([&] { return callbacks > previousCallbacks; });
    const auto afterMiss = calls.load();
    TEST_CHECK(resolver.resolveOrRequest("missing", 32).empty());
    loop.pump(20ms);
    TEST_CHECK(calls == afterMiss);

    for (std::size_t i = 0; i < AsyncIconResolver::kMaxCacheEntries + 1; ++i) {
      const auto name = "cache-" + std::to_string(i);
      loop.until([&] { return resolver.resolveOrRequest(name, 32) == "/icons/" + name; });
    }
    const auto beforeEvictedLookup = calls.load();
    TEST_CHECK(resolver.resolveOrRequest("cache-0", 32).empty());
    loop.until([&] { return resolver.resolveOrRequest("cache-0", 32) == "/icons/cache-0"; });
    TEST_CHECK(calls == beforeEvictedLookup + 1);
  }

  void destructionJoinsOwnedWorkerAndDropsLateUiCallbacks() {
    ResponsiveLoop loop;
    IoGate io;
    int callbacks = 0;
    auto resolver = std::make_unique<AsyncIconResolver>([&](const std::string&, int) {
      io.enter();
      return "/late-icon";
    });
    resolver->setReadyCallback([&] { ++callbacks; });
    TEST_CHECK(resolver->resolveOrRequest("late", 32).empty());
    loop.until([&] { return io.started(); });
    std::thread release([&] {
      std::this_thread::sleep_for(50ms);
      io.release();
    });
    const auto started = Clock::now();
    resolver.reset();
    TEST_CHECK(Clock::now() - started < 1s);
    release.join();
    loop.pump(20ms);
    TEST_CHECK(callbacks == 0);
  }

  void transientLookupErrorRetriesOnlyOnLaterBinding() {
    ResponsiveLoop loop;
    std::atomic<int> calls{0};
    int callbacks = 0;
    AsyncIconResolver resolver([&](const std::string&, int) -> std::string {
      if (++calls == 1) {
        throw std::runtime_error("controlled temporary lookup failure");
      }
      return "/recovered-same-icon";
    });
    resolver.setReadyCallback([&] {
      if (++callbacks == 1) {
        for (int i = 0; i < 128; ++i) {
          TEST_CHECK(resolver.resolveOrRequest("temporary-error", 32).empty());
        }
      }
    });
    TEST_CHECK(resolver.resolveOrRequest("temporary-error", 32).empty());
    loop.until([&] { return callbacks == 1; });
    loop.pump(200ms);
    TEST_CHECK(calls == 1 && callbacks == 1);
    loop.until([&] { return resolver.resolveOrRequest("temporary-error", 32) == "/recovered-same-icon"; }, 1500ms);
    TEST_CHECK(calls == 2 && callbacks == 2);
  }

  void absoluteMissDoesNotSelfRequeueButLaterBindingDiscoversCreatedFile() {
    TemporaryDirectory directory;
    const auto theme = directory.path / "icons/hicolor";
    const auto generic = theme / "scalable/apps/application-x-executable.svg";
    const auto later = directory.path / "later-absolute-icon.svg";
    std::filesystem::create_directories(generic.parent_path());
    std::ofstream(
        theme / "index.theme"
    ) << "[Icon Theme]\nDirectories=scalable/apps\n[scalable/apps]\nSize=64\nType=Scalable\n";
    std::ofstream(generic) << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\"/>";
    ScopedEnvironment home("HOME", directory.path.string());
    ScopedEnvironment dataHome("XDG_DATA_HOME", directory.path.string());
    ScopedEnvironment dataDirs("XDG_DATA_DIRS", directory.path.string());
    // This is a separate miss/retry lifecycle test. The earlier cold-start
    // test deliberately performs no main-thread theme or lookup preparation.
    (void)IconResolver::checkThemeChanged();
    ResponsiveLoop loop;
    int callbacks = 0;
    AsyncIconResolver resolver;
    resolver.setReadyCallback([&] {
      ++callbacks;
      // Real virtualized row rebinding must not immediately create an endless
      // completion/requery loop when the original absolute path is missing.
      for (int i = 0; i < 128; ++i) {
        TEST_CHECK(resolver.resolveOrRequest(later.string(), 32) == generic.string());
      }
    });
    TEST_CHECK(resolver.resolveOrRequest(later.string(), 32).empty());
    loop.until([&] { return callbacks != 0; });
    TEST_CHECK(resolver.resolveOrRequest(later.string(), 32) == generic.string());
    loop.pump(200ms);
    TEST_CHECK(callbacks == 1);
    resolver.setReadyCallback([&] { ++callbacks; });
    std::ofstream(later) << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\"/>";
    loop.until([&] { return resolver.resolveOrRequest(later.string(), 32) == later.string(); }, 1500ms);
    TEST_CHECK(callbacks == 2);
  }

  void themeChecksDoNotHoldEventLoopAndFailuresCanRecover() {
    ResponsiveLoop loop;
    IoGate io;
    std::atomic<int> checks{0};
    int callbacks = 0;
    const auto mainThread = std::this_thread::get_id();
    IconThemePollSource source(
        [&] {
          const int check = ++checks;
          TEST_CHECK(std::this_thread::get_id() != mainThread);
          if (check == 1) {
            io.enter();
            throw std::runtime_error("controlled theme metadata failure");
          }
          return true;
        },
        1ms
    );
    source.setChangeCallback([&] {
      TEST_CHECK(std::this_thread::get_id() == mainThread);
      ++callbacks;
    });
    std::vector<pollfd> unused;
    loop.until([&] {
      source.dispatch(unused, 0);
      return io.started();
    });
    for (int i = 0; i < 100; ++i) {
      source.dispatch(unused, 0);
    }
    loop.checkStatus();
    TEST_CHECK(checks == 1);
    io.release();
    loop.until([&] {
      source.dispatch(unused, 0);
      return callbacks != 0;
    });
    TEST_CHECK(checks >= 2);
  }

  void actualSettingsReadsUseOwningPrivateContextsAndFinalizeBeforeThreadExit() {
    TEST_CHECK(::getenv("GSETTINGS_BACKEND") != nullptr);
    TEST_CHECK(std::string_view(::getenv("GSETTINGS_BACKEND")) == "memory");
    TEST_CHECK(::getenv("GSETTINGS_SCHEMA_DIR") != nullptr);
    TemporaryDirectory directory;
    const auto createTheme = [&](const std::string& name) {
      const auto root = directory.path / "icons" / name;
      std::filesystem::create_directories(root / "scalable/apps");
      std::ofstream(root / "index.theme")
          << "[Icon Theme]\nDirectories=scalable/apps\n[scalable/apps]\nSize=64\nType=Scalable\n";
    };
    createTheme("async-settings-before-cold");
    createTheme("async-settings-first");
    for (int i = 1; i <= 16; ++i) {
      createTheme("async-settings-updated-" + std::to_string(i));
    }
    ScopedEnvironment home("HOME", directory.path.string());
    ScopedEnvironment dataHome("XDG_DATA_HOME", directory.path.string());
    ScopedEnvironment dataDirs("XDG_DATA_DIRS", directory.path.string());
    ResponsiveLoop loop;
    auto* writer = g_settings_new("org.gnome.desktop.interface");
    TEST_CHECK(writer != nullptr);
    TEST_CHECK(g_settings_set_string(writer, "icon-theme", "async-settings-before-cold"));
    settings_probe::Probe probe;
    std::atomic<int> firstChecks{0};
    std::atomic<int> finished{0};
    std::atomic<int> themePhase{0};
    constexpr int updates = 16;
    const auto worker = [&] {
      settings_probe::active = &probe;
      settings_probe::holdFirst = true;
      (void)IconResolver::checkThemeChanged();
      ++firstChecks;
      for (int phase = 1; phase <= updates; ++phase) {
        const auto deadline = Clock::now() + 2s;
        while (themePhase < phase && Clock::now() < deadline) {
          std::this_thread::sleep_for(1ms);
        }
        TEST_CHECK(themePhase == phase);
        (void)IconResolver::checkThemeChanged();
        ++finished;
      }
      settings_probe::active = nullptr;
    };
    std::thread first(worker);
    std::thread second(worker);
    loop.until([&] {
      std::scoped_lock lock(probe.mutex);
      return probe.records.size() == 2;
    });
    {
      std::scoped_lock lock(probe.mutex);
      TEST_CHECK(probe.records[0]->identity != probe.records[1]->identity);
      TEST_CHECK(probe.records[0]->context != probe.records[1]->context);
      TEST_CHECK(probe.records[0]->owner != probe.records[1]->owner);
      TEST_CHECK(!probe.records[0]->finalized && !probe.records[1]->finalized);
    }
    loop.checkStatus();
    // Change the real backend while both private instances are alive. Their
    // owning contexts must deliver updates and retire all queued target refs.
    TEST_CHECK(g_settings_set_string(writer, "icon-theme", "async-settings-first"));
    {
      std::scoped_lock lock(probe.mutex);
      probe.releaseFirst = true;
      probe.wake.notify_all();
    }
    loop.until([&] { return firstChecks == 2; });
    {
      std::scoped_lock lock(probe.mutex);
      for (const auto& record : probe.records) {
        TEST_CHECK(record->finalized);
        TEST_CHECK(record->reads == std::vector<std::string>{"async-settings-first"});
      }
    }
    for (int phase = 1; phase <= updates; ++phase) {
      const auto theme = "async-settings-updated-" + std::to_string(phase);
      TEST_CHECK(g_settings_set_string(writer, "icon-theme", theme.c_str()));
      themePhase = phase;
      loop.until([&] { return finished == 2 * phase; });
      std::scoped_lock lock(probe.mutex);
      TEST_CHECK(probe.records.size() == static_cast<std::size_t>(2 + 2 * phase));
      for (const auto& record : probe.records) {
        TEST_CHECK(record->finalized && record->reads.size() == 1);
      }
      TEST_CHECK(probe.records[2 * phase]->reads == std::vector<std::string>{theme});
      TEST_CHECK(probe.records[2 * phase + 1]->reads == std::vector<std::string>{theme});
    }
    first.join();
    second.join();
    {
      std::scoped_lock lock(probe.mutex);
      TEST_CHECK(probe.records.size() == 2 + 2 * updates);
      for (const auto& record : probe.records) {
        TEST_CHECK(record->finalized && record->reads.size() == 1);
      }
    }
    TEST_CHECK(IconResolver::activeThemeName() == "async-settings-updated-16");
    TEST_CHECK(g_settings_set_string(writer, "icon-theme", "hicolor"));
    g_object_unref(writer);
  }

  void settingsDestructionWaitsForActualDelayedBackendEnqueue() {
    TemporaryDirectory directory;
    ScopedEnvironment home("HOME", directory.path.string());
    ScopedEnvironment dataHome("XDG_DATA_HOME", directory.path.string());
    ScopedEnvironment dataDirs("XDG_DATA_DIRS", directory.path.string());
    ResponsiveLoop loop;
    settings_probe::Probe probe;
    IoGate read;
    IoGate enqueue;
    probe.afterRead = [&] { read.enter(); };
    std::atomic<bool> checked{false};
    std::thread worker([&] {
      settings_probe::active = &probe;
      (void)IconResolver::checkThemeChanged();
      settings_probe::active = nullptr;
      checked = true;
    });
    loop.until([&] { return read.started(); });
    std::thread writer([&] {
      GMainContext* context = g_main_context_new();
      g_main_context_push_thread_default(context);
      GSettings* settings = g_settings_new("org.gnome.desktop.interface");
      settings_probe::invokeProbe = &probe;
      settings_probe::invokeGate = &enqueue;
      TEST_CHECK(g_settings_set_string(settings, "icon-theme", "async-settings-delayed-enqueue"));
      settings_probe::invokeProbe = nullptr;
      settings_probe::invokeGate = nullptr;
      std::atomic<bool> finalized{false};
      g_object_weak_ref(
          G_OBJECT(settings), [](gpointer data, GObject*) { static_cast<std::atomic<bool>*>(data)->store(true); },
          &finalized
      );
      g_object_unref(settings);
      const auto deadline = Clock::now() + 2s;
      while (!finalized && Clock::now() < deadline) {
        if (!g_main_context_iteration(context, FALSE)) {
          std::this_thread::sleep_for(1ms);
        }
      }
      TEST_CHECK(finalized);
      g_main_context_pop_thread_default(context);
      g_main_context_unref(context);
    });
    loop.until([&] { return enqueue.started(); });
    // Exact GLib backend dispatch retains the target before context_invoke.
    // No test-owned reference pins it. Let the real read return/unref while
    // dispatch is stopped before attaching that retained target's source.
    read.release();
    const auto beats = loop.beats();
    loop.pump(50ms);
    TEST_CHECK(!checked);
    {
      std::scoped_lock lock(probe.mutex);
      TEST_CHECK(probe.records.size() == 1 && !probe.records[0]->finalized);
    }
    loop.checkStatus();
    TEST_CHECK(loop.beats() >= beats + 5);
    enqueue.release();
    loop.until([&] { return checked.load(); });
    worker.join();
    writer.join();
    {
      std::scoped_lock lock(probe.mutex);
      TEST_CHECK(probe.records.size() == 1 && probe.records[0]->finalized);
    }
  }

  void actualResultNotificationAndDiagnosticAllocationFailuresRemainRetryable() {
    for (const bool longKey : {false, true}) {
      ResponsiveLoop loop;
      std::atomic<int> calls{0};
      int callbacks = 0;
      const auto failures = allocation_probe::failures.load();
      const std::string name = longKey ? std::string(256, 'r') : "short";
      AsyncIconResolver resolver([&](const std::string&, int) {
        if (++calls == 1) {
          // Long-key copying exercises real result insertion; a short key and
          // SSO path leave the next allocation to the Deferred notification.
          allocation_probe::failNext = true;
        }
        return std::string("/ok");
      });
      resolver.setReadyCallback([&] { ++callbacks; });
      TEST_CHECK(resolver.resolveOrRequest(name, 32).empty());
      loop.until([&] { return allocation_probe::failures == failures + 1; });
      loop.checkStatus();
      loop.until([&] { return resolver.resolveOrRequest(name, 32) == "/ok"; });
      TEST_CHECK(calls == 2 && callbacks == 1);
    }
    ResponsiveLoop loop;
    std::atomic<int> calls{0};
    const auto failures = allocation_probe::failures.load();
    int callbacks = 0;
    AsyncIconResolver resolver([&](const std::string&, int) {
      if (++calls == 1) {
        allocation_probe::failNext = true;
        throw std::bad_alloc();
      }
      return std::string("/ok");
    });
    resolver.setReadyCallback([&] { ++callbacks; });
    TEST_CHECK(resolver.resolveOrRequest("logger-fault", 32).empty());
    loop.until([&] { return callbacks == 1; });
    TEST_CHECK(allocation_probe::failures == failures + 1);
    TEST_CHECK(resolver.resolveOrRequest("after-logger-fault", 32).empty());
    loop.until([&] { return resolver.resolveOrRequest("after-logger-fault", 32) == "/ok"; });
    TEST_CHECK(calls == 2 && callbacks == 2);
  }

  void themeNotificationAllocationFailureRetainsUndeliveredChange() {
    ResponsiveLoop loop;
    std::atomic<int> calls{0};
    const auto failures = allocation_probe::failures.load();
    int callbacks = 0;
    IconThemePollSource source(
        [&] {
          if (++calls == 1) {
            allocation_probe::failNext = true;
            return true;
          }
          return false;
        },
        1ms
    );
    source.setChangeCallback([&] { ++callbacks; });
    std::vector<pollfd> unused;
    loop.until([&] {
      source.dispatch(unused, 0);
      return callbacks == 1;
    });
    TEST_CHECK(allocation_probe::failures == failures + 1 && calls >= 2);
    loop.checkStatus();
    loop.pump(20ms);
    TEST_CHECK(callbacks == 1);
  }

} // namespace

int main() {
  coldLookupLeavesStatusAndHeartbeatResponsive();
  coalescesAndBoundsBurstWithoutLosingLatestVisibleDemand();
  invalidationRejectsOldGenerationAndCancelPreservesCompletedCache();
  cacheHasPositiveAndNegativeBoundsAndWorkerSurvivesFailedLookup();
  destructionJoinsOwnedWorkerAndDropsLateUiCallbacks();
  transientLookupErrorRetriesOnlyOnLaterBinding();
  absoluteMissDoesNotSelfRequeueButLaterBindingDiscoversCreatedFile();
  themeChecksDoNotHoldEventLoopAndFailuresCanRecover();
  actualSettingsReadsUseOwningPrivateContextsAndFinalizeBeforeThreadExit();
  settingsDestructionWaitsForActualDelayedBackendEnqueue();
  actualResultNotificationAndDiagnosticAllocationFailuresRemainRetryable();
  themeNotificationAllocationFailureRetainsUndeliveredChange();
  std::println("launcher_async_metadata: 12 cold I/O, responsiveness and lifecycle contracts passed");
}
