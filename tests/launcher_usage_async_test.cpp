#include "launcher/usage_tracker.h"
#include "tests/async_metadata_test_support.h"

#include <atomic>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {

  using namespace noctalia::test::metadata;
  using Json = nlohmann::json;

  void writeJson(const std::filesystem::path& path, const Json& value) {
    std::ofstream file(path);
    file << value.dump() << '\n';
    TEST_CHECK(file.good());
  }

  Json readJson(const std::filesystem::path& path) {
    std::ifstream file(path);
    TEST_CHECK(file.good());
    return Json::parse(file);
  }

  std::string readBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    TEST_CHECK(file.good());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }

  void seed(const std::filesystem::path& directory) {
    writeJson(directory / "usage_counts.json", {{"apps", {{"existing", 7}, {"older", 2}}}, {"other", {{"kept", 4}}}});
    writeJson(directory / "recently_used.json", {{"apps", {"existing", "older"}}, {"other", {"kept"}}});
  }

  void coldHistoryLoadKeepsGettersStatusAndHeartbeatResponsive() {
    TemporaryDirectory directory;
    seed(directory.path);
    ResponsiveLoop loop;
    IoGate io;
    const auto mainThread = std::this_thread::get_id();
    UsageTracker tracker(directory.path.string(), {.beforeLoad = [&] { io.enter(); }});
    int publications = 0;
    tracker.setLoadedCallback([&] {
      TEST_CHECK(std::this_thread::get_id() == mainThread);
      TEST_CHECK(tracker.loaded());
      ++publications;
    });
    loop.until([&] { return io.started(); });
    TEST_CHECK(io.worker() != mainThread);
    const auto started = Clock::now();
    for (int i = 0; i < 256; ++i) {
      TEST_CHECK(!tracker.loaded());
      TEST_CHECK(tracker.getCount("apps", "existing") == 0);
      TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "existing") == 0);
      TEST_CHECK(tracker.getRecentlyUsedCount("apps") == 0);
    }
    TEST_CHECK(Clock::now() - started < 100ms);
    const auto beats = loop.beats();
    loop.checkStatus();
    loop.pump(200ms);
    loop.checkStatus();
    TEST_CHECK(loop.beats() >= beats + 15);
    TEST_CHECK(loop.maxHeartbeatGap() < 100ms);
    TEST_CHECK(publications == 0);
    io.release();
    loop.until([&] { return tracker.loaded(); });
    TEST_CHECK(publications == 1);
    TEST_CHECK(tracker.getCount("apps", "existing") == 7);
    TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "existing") == 2);
    TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "older") == 1);
    TEST_CHECK(tracker.getRecentlyUsedCount("apps") == 2);
  }

  void recordBeforeLoadMergesBaselineCountsAndUniqueRecentOrder() {
    TemporaryDirectory directory;
    seed(directory.path);
    ResponsiveLoop loop;
    IoGate io;
    {
      UsageTracker tracker(directory.path.string(), {.beforeLoad = [&] { io.enter(); }});
      loop.until([&] { return io.started(); });
      tracker.record("apps", "existing");
      tracker.record("apps", "existing");
      tracker.record("apps", "new");
      tracker.record("other", "new-other");
      TEST_CHECK(tracker.getCount("apps", "existing") == 2);
      TEST_CHECK(tracker.getCount("apps", "new") == 1);
      io.release();
      loop.until([&] { return tracker.loaded(); });
      TEST_CHECK(tracker.getCount("apps", "existing") == 9);
      TEST_CHECK(tracker.getCount("apps", "older") == 2);
      TEST_CHECK(tracker.getCount("other", "kept") == 4);
      TEST_CHECK(tracker.getRecentlyUsedCount("apps") == 3);
      TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "new") == 3);
      TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "existing") == 2);
      TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "older") == 1);
    }
    const auto counts = readJson(directory.path / "usage_counts.json");
    const auto recent = readJson(directory.path / "recently_used.json");
    TEST_CHECK(counts["apps"]["existing"] == 9 && counts["apps"]["new"] == 1);
    TEST_CHECK(counts["other"]["kept"] == 4 && counts["other"]["new-other"] == 1);
    TEST_CHECK(recent["apps"] == Json({"new", "existing", "older"}));
    TEST_CHECK(recent["other"] == Json({"new-other", "kept"}));
  }

  void clearBeforeLoadCannotResurrectOldCountsOrRecency() {
    TemporaryDirectory directory;
    seed(directory.path);
    ResponsiveLoop loop;
    IoGate io;
    {
      UsageTracker tracker(directory.path.string(), {.beforeLoad = [&] { io.enter(); }});
      loop.until([&] { return io.started(); });
      tracker.record("apps", "discarded");
      tracker.clear();
      tracker.record("apps", "fresh");
      io.release();
      loop.until([&] { return tracker.loaded(); });
      TEST_CHECK(tracker.getCount("apps", "fresh") == 1);
      TEST_CHECK(tracker.getCount("apps", "existing") == 0);
      TEST_CHECK(tracker.getCount("apps", "discarded") == 0);
      TEST_CHECK(tracker.getCount("other", "kept") == 0);
      TEST_CHECK(tracker.getRecentlyUsedCount("apps") == 1);
      TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "fresh") == 1);
    }
    TEST_CHECK(readJson(directory.path / "usage_counts.json") == Json({{"apps", {{"fresh", 1}}}}));
    TEST_CHECK(readJson(directory.path / "recently_used.json") == Json({{"apps", {"fresh"}}}));
  }

  void repeatedWritesBeforeMainAdoptionMergeOnlyTheOriginalBaseline() {
    TemporaryDirectory directory;
    writeJson(directory.path / "usage_counts.json", {{"apps", {{"existing", 5}}}});
    writeJson(directory.path / "recently_used.json", {{"apps", {"existing"}}});
    ResponsiveLoop loop;
    IoGate load;
    IoGate firstSave;
    std::atomic<int> saves{0};
    {
      UsageTracker tracker(
          directory.path.string(),
          {.beforeLoad = [&] { load.enter(); },
           .beforeSave =
               [&] {
                 if (++saves == 1) {
                   firstSave.enter();
                 }
               }}
      );
      loop.until([&] { return load.started(); });
      tracker.record("apps", "existing");
      tracker.record("apps", "existing");
      load.release();
      // Intentionally withhold main-thread callback adoption while the real
      // worker loads the baseline and starts its first save. Later revisions
      // must merge with original count five, not a file already containing two.
      const auto deadline = Clock::now() + 2s;
      while (!firstSave.started() && Clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
      }
      TEST_CHECK(firstSave.started() && !tracker.loaded());
      for (int i = 0; i < 3; ++i) {
        tracker.record("apps", "existing");
      }
      TEST_CHECK(tracker.getCount("apps", "existing") == 5);
      firstSave.release();
    }
    TEST_CHECK(saves == 2);
    TEST_CHECK(readJson(directory.path / "usage_counts.json")["apps"]["existing"] == 10);
    TEST_CHECK(readJson(directory.path / "recently_used.json")["apps"] == Json({"existing"}));
  }

  void blockedWriterCoalescesToLatestStateWithoutBlockingMainLoop() {
    TemporaryDirectory directory;
    seed(directory.path);
    ResponsiveLoop loop;
    IoGate io;
    std::atomic<int> saves{0};
    {
      UsageTracker tracker(directory.path.string(), {.beforeSave = [&] {
                             if (++saves == 1) {
                               io.enter();
                             }
                           }});
      loop.until([&] { return tracker.loaded(); });
      tracker.record("apps", "first-write");
      loop.until([&] { return io.started(); });
      const auto started = Clock::now();
      for (int i = 0; i < 256; ++i) {
        tracker.record("apps", "burst-" + std::to_string(i));
      }
      TEST_CHECK(Clock::now() - started < 100ms);
      loop.checkStatus();
      const auto beats = loop.beats();
      loop.pump(200ms);
      loop.checkStatus();
      TEST_CHECK(loop.beats() >= beats + 15);
      TEST_CHECK(loop.maxHeartbeatGap() < 100ms);
      TEST_CHECK(saves == 1);
      TEST_CHECK(tracker.getRecentlyUsedCount("apps") == 20);
      TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "burst-255") == 20);
      TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "burst-236") == 1);
      TEST_CHECK(tracker.getRecentlyUsedIndex("apps", "burst-235") == 0);
      io.release();
    }
    TEST_CHECK(saves == 2);
    const auto counts = readJson(directory.path / "usage_counts.json");
    const auto recent = readJson(directory.path / "recently_used.json");
    TEST_CHECK(counts["apps"]["existing"] == 7 && counts["apps"]["first-write"] == 1);
    TEST_CHECK(counts["apps"]["burst-0"] == 1 && counts["apps"]["burst-255"] == 1);
    TEST_CHECK(recent["apps"].size() == 20);
    TEST_CHECK(recent["apps"][0] == "burst-255" && recent["apps"][19] == "burst-236");
  }

  void loadFailurePreservesFilesAndNextAcceptedRevisionRetries() {
    TemporaryDirectory directory;
    seed(directory.path);
    const auto oldCounts = readJson(directory.path / "usage_counts.json");
    const auto oldRecent = readJson(directory.path / "recently_used.json");
    ResponsiveLoop loop;
    std::atomic<int> loads{0};
    {
      UsageTracker tracker(directory.path.string(), {.beforeLoad = [&] {
                             if (++loads == 1) {
                               throw std::runtime_error("controlled history read failure");
                             }
                           }});
      loop.until([&] { return loads == 1; });
      loop.pump(20ms);
      TEST_CHECK(!tracker.loaded());
      TEST_CHECK(readJson(directory.path / "usage_counts.json") == oldCounts);
      TEST_CHECK(readJson(directory.path / "recently_used.json") == oldRecent);
      tracker.record("apps", "after-load-error");
      loop.until([&] { return tracker.loaded(); });
      TEST_CHECK(loads == 2);
      TEST_CHECK(tracker.getCount("apps", "existing") == 7);
      TEST_CHECK(tracker.getCount("apps", "after-load-error") == 1);
    }
    const auto counts = readJson(directory.path / "usage_counts.json");
    TEST_CHECK(counts["apps"]["existing"] == 7 && counts["apps"]["after-load-error"] == 1);
  }

  void saveFailurePreservesPriorFilesThenFlushesAllAcceptedChanges() {
    TemporaryDirectory directory;
    seed(directory.path);
    const auto oldCounts = readJson(directory.path / "usage_counts.json");
    const auto oldRecent = readJson(directory.path / "recently_used.json");
    ResponsiveLoop loop;
    std::atomic<int> saves{0};
    {
      UsageTracker tracker(directory.path.string(), {.beforeSave = [&] {
                             if (++saves == 1) {
                               throw std::runtime_error("controlled history write failure");
                             }
                           }});
      loop.until([&] { return tracker.loaded(); });
      tracker.record("apps", "before-save-error");
      loop.until([&] { return saves == 1; });
      loop.pump(20ms);
      TEST_CHECK(readJson(directory.path / "usage_counts.json") == oldCounts);
      TEST_CHECK(readJson(directory.path / "recently_used.json") == oldRecent);
      tracker.record("apps", "after-save-error");
    }
    TEST_CHECK(saves == 2);
    const auto counts = readJson(directory.path / "usage_counts.json");
    const auto recent = readJson(directory.path / "recently_used.json");
    TEST_CHECK(counts["apps"]["existing"] == 7);
    TEST_CHECK(counts["apps"]["before-save-error"] == 1 && counts["apps"]["after-save-error"] == 1);
    TEST_CHECK(recent["apps"] == Json({"after-save-error", "before-save-error", "existing", "older"}));
  }

  void actualUnreadableExistingHistoryPreservesFilesUntilRecovery() {
    TemporaryDirectory directory;
    seed(directory.path);
    const auto countsPath = directory.path / "usage_counts.json";
    const auto backup = directory.path / "original-counts.json";
    const auto oldRecent = readJson(directory.path / "recently_used.json");
    const auto oldRecentBytes = readBytes(directory.path / "recently_used.json");
    const auto oldCountsBytes = readBytes(countsPath);
    std::filesystem::rename(countsPath, backup);
    std::filesystem::create_directory(countsPath);
    std::ofstream(countsPath / "marker") << "existing unreadable history";
    ResponsiveLoop loop;
    std::atomic<int> loads{0};
    std::atomic<int> saves{0};
    {
      UsageTracker tracker(directory.path.string(), {.beforeLoad = [&] { ++loads; }, .beforeSave = [&] { ++saves; }});
      loop.until([&] { return loads == 1; });
      loop.pump(20ms);
      TEST_CHECK(!tracker.loaded());
      tracker.record("apps", "during-read-error");
      loop.until([&] { return loads == 2; });
      loop.pump(100ms);
      TEST_CHECK(loads == 2 && saves == 0 && !tracker.loaded());
      TEST_CHECK(std::filesystem::is_directory(countsPath));
      TEST_CHECK(std::filesystem::is_regular_file(countsPath / "marker"));
      TEST_CHECK(readJson(directory.path / "recently_used.json") == oldRecent);
      TEST_CHECK(readBytes(directory.path / "recently_used.json") == oldRecentBytes);
      TEST_CHECK(readBytes(backup) == oldCountsBytes);
      TEST_CHECK(readJson(backup)["apps"]["existing"] == 7);
      std::filesystem::remove_all(countsPath);
      std::filesystem::rename(backup, countsPath);
      tracker.record("apps", "after-read-recovery");
      loop.until([&] { return tracker.loaded(); });
      TEST_CHECK(loads == 3);
      TEST_CHECK(tracker.getCount("apps", "existing") == 7);
      TEST_CHECK(tracker.getCount("apps", "during-read-error") == 1);
      TEST_CHECK(tracker.getCount("apps", "after-read-recovery") == 1);
    }
    const auto counts = readJson(countsPath);
    TEST_CHECK(counts["apps"]["existing"] == 7);
    TEST_CHECK(counts["apps"]["during-read-error"] == 1 && counts["apps"]["after-read-recovery"] == 1);
    TEST_CHECK(
        readJson(directory.path / "recently_used.json")["apps"]
        == Json({"after-read-recovery", "during-read-error", "existing", "older"})
    );
  }

  void destructionFlushesPreloadOperationsAndDropsLateCallbacks() {
    TemporaryDirectory directory;
    seed(directory.path);
    ResponsiveLoop loop;
    IoGate io;
    int publications = 0;
    auto tracker = std::make_unique<UsageTracker>(directory.path.string(), UsageTracker::IoHooks{.beforeLoad = [&] {
                                                    io.enter();
                                                  }});
    tracker->setLoadedCallback([&] { ++publications; });
    loop.until([&] { return io.started(); });
    tracker->record("apps", "before-destruction");
    std::thread release([&] {
      std::this_thread::sleep_for(50ms);
      io.release();
    });
    const auto started = Clock::now();
    tracker.reset();
    TEST_CHECK(Clock::now() - started < 1s);
    release.join();
    loop.pump(20ms);
    TEST_CHECK(publications == 0);
    const auto counts = readJson(directory.path / "usage_counts.json");
    TEST_CHECK(counts["apps"]["existing"] == 7 && counts["apps"]["before-destruction"] == 1);
    TEST_CHECK(readJson(directory.path / "recently_used.json")["apps"][0] == "before-destruction");
  }

  void repeatedHistoryLifetimesReleaseOwnedWorkersAndDescriptors() {
    TemporaryDirectory directory;
    seed(directory.path);
    ResponsiveLoop loop;
    const auto count = [](const char* path) {
      return static_cast<std::size_t>(
          std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator{})
      );
    };
    const auto descriptors = count("/proc/self/fd");
    const auto threads = count("/proc/self/task");
    for (int i = 0; i < 16; ++i) {
      UsageTracker tracker(directory.path.string());
      loop.until([&] { return tracker.loaded(); });
      tracker.record("apps", "iteration");
    }
    TEST_CHECK(count("/proc/self/fd") == descriptors);
    TEST_CHECK(count("/proc/self/task") == threads);
    TEST_CHECK(readJson(directory.path / "usage_counts.json")["apps"]["iteration"] == 16);
  }

} // namespace

int main() {
  coldHistoryLoadKeepsGettersStatusAndHeartbeatResponsive();
  recordBeforeLoadMergesBaselineCountsAndUniqueRecentOrder();
  repeatedWritesBeforeMainAdoptionMergeOnlyTheOriginalBaseline();
  clearBeforeLoadCannotResurrectOldCountsOrRecency();
  blockedWriterCoalescesToLatestStateWithoutBlockingMainLoop();
  loadFailurePreservesFilesAndNextAcceptedRevisionRetries();
  saveFailurePreservesPriorFilesThenFlushesAllAcceptedChanges();
  actualUnreadableExistingHistoryPreservesFilesUntilRecovery();
  destructionFlushesPreloadOperationsAndDropsLateCallbacks();
  repeatedHistoryLifetimesReleaseOwnedWorkersAndDescriptors();
  std::println("launcher_usage_async: 10 cold history, responsiveness, persistence and lifecycle contracts passed");
}
