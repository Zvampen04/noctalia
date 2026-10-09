#include "shell/bar/widgets/system_update_status.h"
#include <cassert>

int main() {
  using system_update_status::State;
  using nlohmann::json;
  const auto empty = json::object();
  const json current = {{"status", "success"}, {"phase", "up-to-date"}};
  const json failed = {{"status", "failed"}};
  const json unavailable = {{"items", {{{"status", "unavailable"}}}}};
  const json selected = {{"items", {{{"status", "selected"}}}}};
  const json running = {{"status", "running"}};
  assert(system_update_status::classify(current, empty, empty, 100) == State::Current);
  assert(system_update_status::classify(current, selected, empty, 100) == State::Current);
  assert(system_update_status::classify(current, unavailable, empty, 100) == State::Current);
  assert(system_update_status::classify(failed, selected, empty, 100) == State::Failed);
  assert(system_update_status::classify(running, unavailable, empty, 100) == State::Failed);
  auto partial = current;
  partial["items"] = unavailable["items"];
  assert(system_update_status::classify(partial, empty, empty, 100) == State::Failed);
  assert(system_update_status::classify(empty, selected, empty, 100) == State::Available);
  assert(system_update_status::classify(current, empty, {{"revision", "candidate"}}, 100) == State::Available);
  const json deferred = {{"items", {{{"status", "deferred"}, {"deferred_until_epoch", 101}}}}};
  assert(system_update_status::classify(current, deferred, empty, 100) == State::Current);
  assert(system_update_status::classify(current, deferred, empty, 101) == State::Available);
  assert(system_update_status::classify({{"status", "success"}, {"reboot_required", true}}, empty, empty, 100) == State::Available);
  assert(system_update_status::classify(empty, empty, empty, 100) == State::Unknown);
  assert(system_update_status::classify({{"status", 1}, {"items", false}}, empty, empty, 100) == State::Unknown);
  system_update_status::Tracker tracker;
  assert(tracker.update(failed, empty, empty, 100) == State::Failed);
  assert(tracker.update(running, empty, empty, 100) == State::Failed);
  assert(tracker.update(current, empty, empty, 100) == State::Current);
  assert(tracker.update(partial, empty, empty, 100) == State::Failed);
  assert(system_update_status::color(State::Current) == "#ffffff");
  assert(system_update_status::color(State::Available) == "#ffff00");
  assert(system_update_status::color(State::Failed) == "#ff0000");
}
