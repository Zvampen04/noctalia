// Locks the atomic set-and-clear contract of ConfigService::mutateOverrides. Switching a calendar
// account to another provider writes the new keys and retires the ones the new provider does not own.
// Doing that as a clear followed by a set publishes an intermediate account that is neither shape and
// fails schema validation, so the mutation must reach the config exactly once.

#include "config/config_service.h"
#include "config/config_types.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

  int g_failures = 0;

  void expect(bool condition, const char* message) {
    if (!condition) {
      std::println(stderr, "config_override_mutation: FAIL: {}", message);
      ++g_failures;
    }
  }

  void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
  }

  const CalendarConfig::Account* findAccount(const Config& config, std::string_view id) {
    const auto it = std::ranges::find(config.calendar.accounts, id, &CalendarConfig::Account::id);
    return it == config.calendar.accounts.end() ? nullptr : &*it;
  }

} // namespace

int main() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / ("noctalia-override-mutation-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  writeFile(root / "config" / "noctalia" / "config.toml", "\n");

  ::setenv("NOCTALIA_CONFIG_HOME", (root / "config").c_str(), 1);
  ::setenv("XDG_STATE_HOME", (root / "state").c_str(), 1);

  const std::vector<std::string> typePath{"calendar", "account", "feed", "type"};
  const std::vector<std::string> serverUrlPath{"calendar", "account", "feed", "server_url"};
  const std::vector<std::string> vdirPath{"calendar", "account", "feed", "path"};

  {
    ConfigService config;

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> create;
    create.emplace_back(std::vector<std::string>{"calendar", "enabled"}, true);
    create.emplace_back(typePath, std::string("ics"));
    create.emplace_back(serverUrlPath, std::string("https://example.com/calendar.ics"));
    expect(config.setOverrides(std::move(create)), "ics account writes");
    expect(config.hasOverride(serverUrlPath), "server_url stored for the ics account");

    int reloads = 0;
    config.addReloadCallback([&reloads]() { ++reloads; }, "mutation-test");

    // Switch the account to a local vdir directory. `server_url` is a hard error on a vdir account and
    // `path` is required, so both edits belong to the same commit.
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> switchToVdir;
    switchToVdir.emplace_back(typePath, std::string("vdir"));
    switchToVdir.emplace_back(vdirPath, std::string("/tmp/noctalia-vdir-mutation-test"));
    expect(config.mutateOverrides(switchToVdir, {serverUrlPath}, nullptr), "provider switch writes");

    expect(reloads == 1, "provider switch reaches the config exactly once");
    expect(!config.hasOverride(serverUrlPath), "server_url retired by the switch");
    expect(config.hasOverride(vdirPath), "path stored by the switch");
    expect(config.lastMutationError().empty(), "provider switch produced no mutation error");

    const CalendarConfig::Account* account = findAccount(config.config(), "feed");
    expect(account != nullptr, "account survives the switch");
    if (account != nullptr) {
      expect(account->type == "vdir", "account type is vdir");
      expect(account->path == "/tmp/noctalia-vdir-mutation-test", "account path is set");
      expect(account->serverUrl.empty(), "account server_url is cleared");
    }

    // A mutation that changes nothing must not commit, so consumers are not woken for a no-op.
    expect(config.mutateOverrides({}, {serverUrlPath}, nullptr), "clearing an absent key succeeds");
    expect(reloads == 1, "no-op mutation does not reach the config");

    std::vector<BarSectionConfig> sections;
    for (int i = 0; i < 4; ++i) {
      BarSectionConfig section;
      section.id = "section-" + std::to_string(i);
      section.widgets = {"clock"};
      section.anchor = i == 0 ? BarCenterAlignment::Start
                              : i == 3 ? BarCenterAlignment::End : BarCenterAlignment::Center;
      section.offset = static_cast<float>(i * 12);
      sections.push_back(std::move(section));
    }
    const std::vector<std::string> sectionPath{"bar", "default", "section"};
    expect(config.setOverride(sectionPath, sections), "ordered four-section override writes");
    expect(config.hasOverride(sectionPath), "section vector is retained as one atomic override");
    const auto barIt = std::ranges::find(config.config().bars, std::string("default"), &BarConfig::name);
    expect(barIt != config.config().bars.end(), "default bar remains available after section override");
    if (barIt != config.config().bars.end()) {
      expect(barIt->sections == sections, "ordered section vector round-trips into effective config");
    }

    const std::vector<BarWidgetPlacementConfig> placements{
        {.id = "meter-a", .widget = "cpu", .foreground = colorSpecFromRole(ColorRole::OnPrimary)},
        {.id = "meter-b", .widget = "cpu", .iconForeground = colorSpecFromRole(ColorRole::Primary)},
    };
    const std::vector<std::string> placementPath{"bar", "default", "widget_placement"};
    expect(config.setOverride(placementPath, placements), "stable widget placement override writes");
    const auto placementBar = std::ranges::find(config.config().bars, std::string("default"), &BarConfig::name);
    expect(placementBar != config.config().bars.end() && placementBar->widgetPlacements == placements,
           "same-type widget placements retain distinct stable IDs");
    expect(config.setOverride({"bar", "default", "start"},
                              std::vector<std::string>{"@widget:meter-b", "@widget:meter-a"}),
           "placement tokens reorder independently of widget type");
    const auto reorderedBar = std::ranges::find(config.config().bars, std::string("default"), &BarConfig::name);
    expect(reorderedBar != config.config().bars.end()
            && reorderedBar->startWidgets == std::vector<std::string>{"@widget:meter-b", "@widget:meter-a"}
            && reorderedBar->widgetPlacements == placements,
           "reorder preserves placement identity and material target ownership");
    auto duplicatePlacements = placements;
    duplicatePlacements.back().id = duplicatePlacements.front().id;
    expect(!config.setOverride(placementPath, duplicatePlacements),
           "duplicate stable widget placement IDs are rejected atomically");

    auto duplicate = sections;
    duplicate.back().id = duplicate.front().id;
    expect(!config.setOverride(sectionPath, duplicate), "duplicate section ids are rejected");
    auto emptyId = sections;
    emptyId.front().id.clear();
    expect(!config.setOverride(sectionPath, emptyId), "empty section ids are rejected");
    const auto afterRejected = std::ranges::find(config.config().bars, std::string("default"), &BarConfig::name);
    expect(afterRejected != config.config().bars.end() && afterRejected->sections == sections
            && config.hasOverride(sectionPath),
           "invalid section batches leave the prior ordered override intact");

    const std::vector<std::string> monitorSectionPath{"bar", "default", "monitor", "DP-1", "section"};
    expect(config.setOverride(monitorSectionPath, std::vector<BarSectionConfig>{}),
           "explicit empty monitor section replacement writes");
    expect(config.hasOverride(monitorSectionPath),
           "explicit empty monitor section replacement is distinct from inheritance");
  }

  std::filesystem::remove_all(root);

  if (g_failures == 0) {
    std::println("config_override_mutation_test passed");
    return 0;
  }
  return 1;
}
