#include "core/inotify/inotify.h"
#include "system/desktop_entry_cache.h"
#include "tests/async_metadata_test_support.h"

#include <atomic>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>

namespace tracking_failure {
  // Only the probe's main thread arms this fault, after libc has successfully
  // created a new kernel watch. Worker and general allocations are unaffected.
  thread_local bool armAfterWatch = false;
  thread_local bool failNextAllocation = false;
  std::atomic<int> failedAllocations{0};
} // namespace tracking_failure

void* operator new(std::size_t size) {
  if (std::exchange(tracking_failure::failNextAllocation, false)) {
    ++tracking_failure::failedAllocations;
    throw std::bad_alloc();
  }
  if (void* result = std::malloc(std::max<std::size_t>(size, 1))) {
    return result;
  }
  throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

extern "C" int inotify_add_watch(int fd, const char* path, std::uint32_t mask) noexcept {
  using AddWatch = int (*)(int, const char*, std::uint32_t);
  static const auto real = reinterpret_cast<AddWatch>(::dlsym(RTLD_NEXT, "inotify_add_watch"));
  TEST_CHECK(real != nullptr);
  const int watch = real(fd, path, mask);
  if (watch >= 0 && std::exchange(tracking_failure::armAfterWatch, false)) {
    tracking_failure::failNextAllocation = true;
  }
  return watch;
}

namespace {

  using namespace noctalia::test::metadata;
  using noctalia::detail::DesktopEntryCache;
  using noctalia::detail::DesktopEntryEnvironment;

  void writeEntry(const std::filesystem::path& directory, const std::string& id, const std::string& name) {
    std::filesystem::create_directories(directory / "applications");
    std::ofstream entry(directory / "applications" / (id + ".desktop"));
    entry
        << "[Desktop Entry]\nType=Application\nName="
        << name
        << "\nName[ru]="
        << name
        << " Russian\nName[de]="
        << name
        << " German\nExec=/bin/true\n";
    TEST_CHECK(entry.good());
  }

  DesktopEntryEnvironment environmentFor(const std::filesystem::path& root) {
    return {
        .home = root.string(),
        .path = "/bin",
        .dataHome = root.string(),
        .dataDirs = (root / "empty").string(),
        .currentDesktop = "Hyprland"
    };
  }

  bool contains(const std::vector<DesktopEntry>& entries, const std::string& id, const std::string& name) {
    const auto found = std::ranges::find(entries, id, &DesktopEntry::id);
    return found != entries.end() && found->name == name;
  }

  void coldScanLeavesGettersStatusAndHeartbeatResponsive() {
    TemporaryDirectory directory;
    writeEntry(directory.path, "async-probe-one", "First");
    writeEntry(directory.path, "async-probe-two", "Second");
    ResponsiveLoop loop;
    IoGate io;
    const auto mainThread = std::this_thread::get_id();
    std::atomic<int> scans{0};
    DesktopEntryCache cache(environmentFor(directory.path), "en", [&] {
      ++scans;
      io.enter();
    });
    int publications = 0;
    auto connection = cache.changed().connect([&] {
      TEST_CHECK(std::this_thread::get_id() == mainThread);
      TEST_CHECK(contains(*cache.entriesSnapshot(), "async-probe-one", "First"));
      TEST_CHECK(contains(cache.entries(), "async-probe-two", "Second"));
      ++publications;
    });
    loop.until([&] { return io.started(); });
    TEST_CHECK(io.worker() != mainThread);
    const auto started = Clock::now();
    for (int i = 0; i < 256; ++i) {
      TEST_CHECK(cache.entries().empty());
      TEST_CHECK(cache.entriesSnapshot()->empty());
      TEST_CHECK(cache.version() == 0);
      cache.checkReload();
    }
    TEST_CHECK(Clock::now() - started < 100ms);
    const auto beatsBefore = loop.beats();
    loop.checkStatus();
    loop.pump(200ms);
    loop.checkStatus();
    TEST_CHECK(loop.beats() >= beatsBefore + 15);
    TEST_CHECK(loop.maxHeartbeatGap() < 100ms);
    TEST_CHECK(scans == 1 && publications == 0);
    io.release();
    loop.until([&] {
      cache.checkReload();
      return publications == 1;
    });
    TEST_CHECK(cache.version() == 1);
    TEST_CHECK(contains(*cache.entriesSnapshot(), "async-probe-one", "First"));
  }

  void generationAndBurstAdoptOnlyLatestEnvironmentAndLanguage() {
    TemporaryDirectory first;
    TemporaryDirectory latest;
    writeEntry(first.path, "async-generation-old", "Old");
    writeEntry(latest.path, "async-generation-new", "New");
    ResponsiveLoop loop;
    IoGate io;
    std::atomic<int> scans{0};
    DesktopEntryCache cache(environmentFor(first.path), "en", [&] {
      if (++scans == 1) {
        io.enter();
      }
    });
    int publications = 0;
    auto connection = cache.changed().connect([&] {
      TEST_CHECK(contains(*cache.entriesSnapshot(), "async-generation-new", "New Russian"));
      TEST_CHECK(
          std::ranges::find(cache.entries(), "async-generation-old", &DesktopEntry::id) == cache.entries().end()
      );
      ++publications;
    });
    loop.until([&] { return io.started(); });
    for (int i = 0; i < 128; ++i) {
      cache.checkSourcesChanged(environmentFor(i % 2 == 0 ? first.path : latest.path));
      cache.setLanguage(i % 2 == 0 ? "de" : "ru");
    }
    loop.checkStatus();
    TEST_CHECK(scans == 1);
    io.release();
    loop.until([&] {
      cache.checkReload();
      return publications != 0;
    });
    TEST_CHECK(publications == 1);
    TEST_CHECK(cache.version() == 1);
    TEST_CHECK(scans == 2);
  }

  void inotifyRefreshPublishesOneCoherentSnapshotAndKeepsOldReadersValid() {
    TemporaryDirectory directory;
    writeEntry(directory.path, "async-coherence", "Original");
    ResponsiveLoop loop;
    IoGate io;
    std::atomic<int> scans{0};
    DesktopEntryCache cache(environmentFor(directory.path), "en", [&] {
      if (++scans == 2) {
        io.enter();
      }
    });
    loop.until([&] {
      cache.checkReload();
      return cache.version() != 0;
    });
    const auto old = cache.entriesSnapshot();
    const auto oldVersion = cache.version();
    TEST_CHECK(contains(*old, "async-coherence", "Original"));
    writeEntry(directory.path, "async-coherence", "Changed");
    loop.until([&] {
      cache.checkReload();
      return io.started();
    });
    TEST_CHECK(cache.version() == oldVersion);
    TEST_CHECK(contains(cache.entries(), "async-coherence", "Original"));
    TEST_CHECK(cache.entriesSnapshot() == old);
    std::atomic<bool> readersDone{false};
    std::thread reader([&] {
      for (int i = 0; i < 1000; ++i) {
        const auto snapshot = cache.entriesSnapshot();
        TEST_CHECK(
            contains(*snapshot, "async-coherence", "Original") || contains(*snapshot, "async-coherence", "Changed")
        );
        TEST_CHECK(contains(*old, "async-coherence", "Original"));
      }
      readersDone = true;
    });
    loop.checkStatus();
    io.release();
    loop.until([&] {
      cache.checkReload();
      return cache.version() > oldVersion && readersDone;
    });
    reader.join();
    TEST_CHECK(contains(*cache.entriesSnapshot(), "async-coherence", "Changed"));
    TEST_CHECK(contains(*old, "async-coherence", "Original"));
  }

  void capturedEnvironmentCannotBeReplacedByConcurrentProcessEnvironment() {
    TemporaryDirectory captured;
    TemporaryDirectory unrelated;
    writeEntry(captured.path, "async-captured", "Captured");
    writeEntry(unrelated.path, "async-unrelated", "Unrelated");
    ScopedEnvironment home("HOME", captured.path.string());
    ScopedEnvironment dataHome("XDG_DATA_HOME", captured.path.string());
    ScopedEnvironment dataDirs("XDG_DATA_DIRS", (captured.path / "empty").string());
    const auto environment = DesktopEntryEnvironment::capture();
    ResponsiveLoop loop;
    IoGate io;
    DesktopEntryCache cache(environment, "en", [&] { io.enter(); });
    loop.until([&] { return io.started(); });
    ScopedEnvironment changedHome("HOME", unrelated.path.string());
    ScopedEnvironment changedDataHome("XDG_DATA_HOME", unrelated.path.string());
    io.release();
    loop.until([&] {
      cache.checkReload();
      return cache.version() != 0;
    });
    TEST_CHECK(contains(cache.entries(), "async-captured", "Captured"));
    TEST_CHECK(std::ranges::find(cache.entries(), "async-unrelated", &DesktopEntry::id) == cache.entries().end());
  }

  void profileGenerationSymlinkChangeRefreshesWithoutBlockingReaders() {
    TemporaryDirectory first;
    TemporaryDirectory second;
    TemporaryDirectory profile;
    writeEntry(first.path, "async-profile", "Old profile");
    writeEntry(second.path, "async-profile", "New profile");
    const auto link = profile.path / "generation";
    std::filesystem::create_directory_symlink(first.path, link);
    const auto environment = environmentFor(link);
    ResponsiveLoop loop;
    IoGate io;
    std::atomic<int> scans{0};
    DesktopEntryCache cache(environment, "en", [&] {
      if (++scans == 2) {
        io.enter();
      }
    });
    loop.until([&] {
      cache.checkReload();
      return cache.version() != 0;
    });
    const auto original = cache.entriesSnapshot();
    TEST_CHECK(contains(*original, "async-profile", "Old profile"));
    const auto replacement = profile.path / "next-generation";
    std::filesystem::create_directory_symlink(second.path, replacement);
    std::filesystem::rename(replacement, link);
    const auto started = Clock::now();
    cache.checkSourcesChanged(environment);
    TEST_CHECK(Clock::now() - started < 100ms);
    loop.until([&] { return io.started(); });
    TEST_CHECK(cache.entriesSnapshot() == original);
    loop.checkStatus();
    io.release();
    loop.until([&] {
      cache.checkReload();
      return contains(cache.entries(), "async-profile", "New profile");
    });
    TEST_CHECK(contains(*original, "async-profile", "Old profile"));
  }

  void failedScanKeepsLastGoodSnapshotAndNextRefreshRecovers() {
    TemporaryDirectory directory;
    writeEntry(directory.path, "async-recovery", "Stable");
    ResponsiveLoop loop;
    std::atomic<int> scans{0};
    DesktopEntryCache cache(environmentFor(directory.path), "en", [&] {
      const auto scan = ++scans;
      if (scan == 2) {
        throw std::runtime_error("controlled cold catalog failure");
      }
      if (scan == 3) {
        // Throwing bad_alloc itself does not use operator new. The next real
        // allocation belongs to catch-path Logger formatting and must also be
        // contained without killing the worker or losing the previous catalog.
        tracking_failure::failNextAllocation = true;
        throw std::bad_alloc();
      }
    });
    loop.until([&] {
      cache.checkReload();
      return cache.version() != 0;
    });
    const auto stable = cache.entriesSnapshot();
    const auto version = cache.version();
    int publications = 0;
    auto connection = cache.changed().connect([&] { ++publications; });
    cache.setLanguage("ru");
    loop.until([&] { return scans >= 2; });
    loop.pump(30ms);
    cache.checkReload();
    TEST_CHECK(cache.entriesSnapshot() == stable);
    TEST_CHECK(cache.version() == version);
    TEST_CHECK(publications == 0);
    const auto failures = tracking_failure::failedAllocations.load();
    cache.setLanguage("fr");
    loop.until([&] { return scans >= 3 && tracking_failure::failedAllocations == failures + 1; });
    loop.pump(30ms);
    cache.checkReload();
    TEST_CHECK(cache.entriesSnapshot() == stable && cache.version() == version && publications == 0);
    cache.setLanguage("de");
    loop.until([&] {
      cache.checkReload();
      return contains(cache.entries(), "async-recovery", "Stable German");
    });
    TEST_CHECK(cache.version() == version + 1);
    TEST_CHECK(publications == 1);
    TEST_CHECK(contains(*stable, "async-recovery", "Stable"));
  }

  void destructionJoinsWorkerAndConnectionsCannotPublishIntoDestroyedOwner() {
    TemporaryDirectory directory;
    writeEntry(directory.path, "async-destroy", "Destroy");
    ResponsiveLoop loop;
    IoGate io;
    int publications = 0;
    auto cache = std::make_unique<DesktopEntryCache>(environmentFor(directory.path), "en", [&] { io.enter(); });
    auto connection = cache->changed().connect([&] { ++publications; });
    const auto empty = cache->entriesSnapshot();
    loop.until([&] { return io.started(); });
    std::thread release([&] {
      std::this_thread::sleep_for(50ms);
      io.release();
    });
    const auto started = Clock::now();
    cache.reset();
    TEST_CHECK(Clock::now() - started < 1s);
    release.join();
    connection.disconnect();
    loop.pump(20ms);
    TEST_CHECK(publications == 0);
    TEST_CHECK(empty->empty());
  }

  void repeatedCatalogLifetimesReleaseWorkersAndWatchDescriptors() {
    TemporaryDirectory directory;
    writeEntry(directory.path, "async-fd-lifetime", "Lifetime");
    ResponsiveLoop loop;
    const auto count = [](const char* path) {
      return static_cast<std::size_t>(
          std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator{})
      );
    };
    const auto descriptors = count("/proc/self/fd");
    const auto threads = count("/proc/self/task");
    for (int i = 0; i < 16; ++i) {
      DesktopEntryCache cache(environmentFor(directory.path), "en");
      loop.until([&] {
        cache.checkReload();
        return contains(cache.entries(), "async-fd-lifetime", "Lifetime");
      });
      TEST_CHECK(cache.watchFd() >= 0);
    }
    TEST_CHECK(count("/proc/self/fd") == descriptors);
    TEST_CHECK(count("/proc/self/task") == threads);
  }

  void trackingAllocationFailurePropagatesAndRemovesOnlyItsNewKernelWatch() {
    TemporaryDirectory directory;
    const auto existing = directory.path / "existing";
    const auto failing = directory.path / "failing";
    std::filesystem::create_directories(existing);
    std::filesystem::create_directories(failing);
    Inotify inotify;
    TEST_CHECK(inotify.fd() >= 0);
    const auto original = inotify.watch(existing, IN_CLOSE_WRITE);
    TEST_CHECK(original.has_value());
    const auto watched = [&] {
      std::ifstream file("/proc/self/fdinfo/" + std::to_string(inotify.fd()));
      TEST_CHECK(file.good());
      std::size_t watches = 0;
      for (std::string line; std::getline(file, line);) {
        if (line.starts_with("inotify wd:")) {
          ++watches;
        }
      }
      return watches;
    };
    TEST_CHECK(watched() == 1);
    tracking_failure::armAfterWatch = true;
    bool caught = false;
    try {
      (void)inotify.watch(failing, IN_CLOSE_WRITE);
    } catch (const std::bad_alloc&) {
      caught = true;
    }
    TEST_CHECK(caught);
    TEST_CHECK(!tracking_failure::armAfterWatch && !tracking_failure::failNextAllocation);
    TEST_CHECK(watched() == 1);
    // The original unrelated watch still works; the removed probe watch can
    // be added successfully after allocation recovers.
    std::ofstream(existing / "event") << "watch still active";
    bool originalEvent = false;
    inotify.drain([&](const inotify_event* event) {
      if (event->wd == *original && (event->mask & IN_CLOSE_WRITE) != 0) {
        originalEvent = true;
      }
    });
    TEST_CHECK(originalEvent);
    TEST_CHECK(inotify.watch(failing, IN_CLOSE_WRITE).has_value());
    TEST_CHECK(watched() == 2);
  }

} // namespace

int main() {
  coldScanLeavesGettersStatusAndHeartbeatResponsive();
  generationAndBurstAdoptOnlyLatestEnvironmentAndLanguage();
  inotifyRefreshPublishesOneCoherentSnapshotAndKeepsOldReadersValid();
  capturedEnvironmentCannotBeReplacedByConcurrentProcessEnvironment();
  profileGenerationSymlinkChangeRefreshesWithoutBlockingReaders();
  failedScanKeepsLastGoodSnapshotAndNextRefreshRecovers();
  destructionJoinsWorkerAndConnectionsCannotPublishIntoDestroyedOwner();
  repeatedCatalogLifetimesReleaseWorkersAndWatchDescriptors();
  trackingAllocationFailurePropagatesAndRemovesOnlyItsNewKernelWatch();
  std::println("desktop_entry_async: 9 cold catalog, responsiveness and lifecycle contracts passed");
}
