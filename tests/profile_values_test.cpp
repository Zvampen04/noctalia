#include "config/profile_values.h"
#include <cassert>
#include <sstream>
using namespace noctalia::profile;
Json encode(const toml::table& table) {
  std::ostringstream out; out << toml::json_formatter{table}; return Json::parse(out.str());
}
int main() {
  const auto committed = toml::parse(R"(
[shell]
font_family = "Original"
[shell.material]
bevel_width = 0
[theme]
name = "Current palette"
[wallpaper]
path = "/new/wallpaper.png"
[audio]
volume_step = 4
[plugin_settings."custom/widget"]
panel_margin = 10
volume = 17
)");
  auto live = committed;
  applyEntries(live, Json::array({
      {{"path", {"shell", "font_family"}}, {"value", "Preview"}},
      {{"path", {"shell", "design"}}, {"value", {{"space_md", 0}}}},
      {{"path", {"plugin_settings", "custom/widget", "panel_margin"}}, {"value", 0}}
  }));
  live["theme"].as_table()->insert_or_assign("name", "Changed during preview");
  live["audio"].as_table()->insert_or_assign("volume_step", 6);
  auto output = live;
  replace(output, committed);
  assert(output["shell"]["font_family"].value<std::string>() == "Original");
  assert(!output["shell"]["design"]);
  assert(output["theme"]["name"].value<std::string>() == "Changed during preview");
  assert(output["audio"]["volume_step"].value<int>() == 6);
  assert(output["wallpaper"]["path"].value<std::string>() == "/new/wallpaper.png");
  assert(output["plugin_settings"]["custom/widget"]["volume"].value<int>() == 17);
  assert(subset(output) == subset(committed));
  assert(!owns({"wallpaper"}) && !owns({"theme", "name"}) && !owns({"audio", "volume_step"}));
  assert(!owns({"shell", "material_override_editor_target"}));
  assert(owns({"shell", "material_overrides"}));
  assert(encode(live)["shell"]["design"]["space_md"] == 0);
  for (const auto& invalid : {Json::array({{{"path", {"theme", "name"}}, {"value", "wrong"}}}),
      Json::array({{{"path", {"shell", "design"}}, {"value", Json::object()}},
                   {{"path", {"shell", "design", "space_md"}}, {"value", 3}}})}) {
    bool rejected = false;
    try { auto copy = committed; applyEntries(copy, invalid); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
  }
  bool conflict = false;
  try { validateExpected(encode(live), Json::array({{{"path", {"shell", "font_family"}}, {"value", "Original"}}})); }
  catch (const std::runtime_error&) { conflict = true; }
  assert(conflict);
  validateExpected(encode(live), Json::array({{{"path", {"shell", "design", "space_md"}}, {"value", 0.0}}}));
  // A mirrors-only request cannot persist appearance values through that path.
  bool escaped = false;
  try { auto copy = committed; applyEntries(copy, Json::array({{{"path", {"shell", "font_family"}}, {"value", "Wrong"}}}), false); }
  catch (const std::runtime_error&) { escaped = true; }
  assert(escaped);
  auto missingReset = committed;
  applyEntries(missingReset, Json::array({{{"path", {"bar", "absent", "radius"}}, {"value", nullptr}}}));
  assert(missingReset == committed);
  auto external = committed;
  external["theme"].as_table()->insert_or_assign("name", "External palette");
  auto local = committed;
  local["audio"].as_table()->insert_or_assign("volume_step", 10);
  auto rebased = external;
  assert(rebaseUnrelated(rebased, committed, local, external));
  assert(rebased["theme"]["name"].value<std::string>() == "External palette");
  assert(rebased["audio"]["volume_step"].value<int>() == 10);
  external["audio"].as_table()->insert_or_assign("volume_step", 11);
  rebased = external;
  assert(!rebaseUnrelated(rebased, committed, local, external));
  auto mirror = committed;
  applyEntries(mirror, Json::array({{{"path", {"plugin_settings", "infinite-desktop/settings", "result"}}, {"value", "Saved"}}}), false);
  assert(subset(mirror) == subset(committed));
}
