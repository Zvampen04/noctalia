#include "ui/controls/slider.h"
// Integration tests use the real ConfigService and persistence implementation.
// They create no shell, renderer, D-Bus service, or host configuration files.
#include "config/config_service.h"
#include "config/profile_scope.h"
#include "shell/greeter/greeter_appearance_sync.h"
#include "shell/greeter/appearance_snapshot.h"
#include "ui/palette.h"
#include "render/scene/node.h"
#include "ui/material_overrides.h"
#include "ui/popup_parent.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/custom_effect_asset_service.h"
#include "shell/settings/settings_content_plugins.h"
#include "scripting/plugin_manifest.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/bar_presentation_setting.h"
#include "ui/controls/bezier_editor.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/input.h"
#include "render/scene/input_area.h"
#include "shell/settings/settings_window.h"
#include "core/deferred_call.h"
#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <limits>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

// Exercise the actual settings controller and its real teardown without a
// compositor. Only service wiring and private UI action dispatch need access;
// the queue, mutations, lifetime tokens and ConfigService are production code.
class SettingsWindowMutationTestAccess {
public:
  static void attach(SettingsWindow& window, ConfigService& service) {
    window.m_config = &service;
    window.m_statusMessage = "fresh window";
    window.m_statusIsError = false;
  }
  static void closeWindow(SettingsWindow& window) { window.destroyWindow(); }
  static bool freshStatus(const SettingsWindow& window) {
    return window.m_statusMessage == "fresh window" && !window.m_statusIsError;
  }
  static void transition(SettingsWindow& window, const std::string& action) {
    window.runProfileTransition(action);
  }
  static bool transitionBusy(const SettingsWindow& window) { return window.m_profileTransitionBusy; }
  static bool errorStatus(const SettingsWindow& window) { return window.m_statusIsError; }
  static std::unique_ptr<Flex> statusRow(SettingsWindow& window) { return window.buildStatusRow(1.0F); }
  static void setRadius(SettingsWindow& window, double radius) {
    window.setSettingOverride({"shell", "design", "radius_xl"}, radius);
  }
  static void setBatch(SettingsWindow& window, std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> values) {
    window.setSettingOverrides(std::move(values));
  }
  static void clearBatch(SettingsWindow& window, std::vector<std::vector<std::string>> paths) {
    window.clearSettingOverrides(std::move(paths));
  }
  static void setInvalidRadius(SettingsWindow& window) {
    window.setSettingOverride({"shell", "design", "radius_xl"}, std::string("invalid"));
  }
  static void queueAllOrdinaryActions(SettingsWindow& window, const std::string& bar) {
    window.setSettingOverride({"shell", "design", "radius_xl"}, 33.0);
    window.setSettingOverrides({{{"shell", "animation", "enabled"}, false}});
    window.clearSettingOverride({"shell", "design", "radius_xl"});
    window.clearSettingOverrides({{"shell", "design", "radius_xl"}});
    window.resetBarLane({"bar", bar, "left"});
    window.renameWidgetInstance("old-widget", "new-widget", {});
    window.createBar("late-bar");
    window.renameBar(bar, "renamed-bar");
    window.deleteBar(bar);
    window.moveBar(bar, 1);
    window.createMonitorOverride(bar, "DP-test");
    window.renameMonitorOverride(bar, "DP-test", "DVI-test");
    window.deleteMonitorOverride(bar, "DP-test");
  }
};

namespace {
using json = nlohmann::json;
namespace fs = std::filesystem;

void check(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

void exactMaterialPlaneAndSnapshotTransport() {
  namespace schema = noctalia::config::schema;
  for (const std::string_view style : {"native", "expressive", "linear", "custom"}) {
    const toml::table table{{"animation", toml::table{{"style", style}, {"enabled", false},
        {"curve_x1", 0.0}, {"curve_y1", -2.0}, {"curve_x2", 1.0}, {"curve_y2", 2.0}}}};
    ShellConfig parsed;
    schema::Diagnostics diag;
    schema::readInto(table, parsed, schema::shellSchema(), "shell", diag);
    check(!diag.hasErrors() && parsed.animation.style == *parseMotionStyle(style) && !parsed.animation.enabled,
        "Motion choice/None failed native schema parsing");
    check(parsed.animation.curveX1 == 0 && parsed.animation.curveY1 == -2
        && parsed.animation.curveX2 == 1 && parsed.animation.curveY2 == 2,
        "Native motion schema lost explicit zero/signed bounds");
  }
  {
    ShellConfig parsed;
    schema::Diagnostics diag;
    schema::readInto(toml::table{{"animation", toml::table{{"style", "bounce"}}}}, parsed,
        schema::shellSchema(), "shell", diag);
    check(diag.hasErrors(), "Unknown motion style was accepted");
  }
  auto testDiscrete = [](std::string_view key, const auto& value, bool accepted) {
    for (const std::string_view scope : {"", "roles", "families", "surfaces"}) {
      toml::table table;
      if (scope.empty()) table.insert("material", toml::table{{key, value}});
      else table.insert("material_overrides", toml::table{{scope,
          toml::table{{"lock", toml::table{{key, value}}}}}});
      ShellConfig shell;
      schema::Diagnostics diag;
      schema::readInto(table, shell, schema::shellSchema(), "shell", diag);
      check(diag.hasErrors() != accepted, std::format("Material mapping acceptance differs for {}.{}={} (expected {})",
          scope.empty() ? "global" : scope, key, value, accepted));
      if (accepted) {
        const auto written = schema::writeTable(shell, schema::shellSchema());
        const auto stored = scope.empty() ? written["material"][key].value<double>()
            : written["material_overrides"][scope]["lock"][key].value<double>();
        check(stored.has_value(), "Explicit material mapping vanished during schema serialization");
        if constexpr (std::is_arithmetic_v<std::decay_t<decltype(value)>>)
          check(*stored == static_cast<double>(value), "Explicit material mapping changed during round trip");
      }
    }
  };
  for (double plane : {-1.0, 0.0, 1.0}) testDiscrete("optical_plane", plane, true);
  for (double invalid : {-2.0, 2.0, -0.5, 0.5, -1.0 + 1e-12, 1.0 - 1e-12, 1e-100})
    testDiscrete("optical_plane", invalid, false);
  for (double mapping : {0.0, 1.0}) testDiscrete("lens_mapping", mapping, true);
  for (double invalid : {-1.0, 2.0, 0.5, 1.0 - 1e-12, 1e-100}) testDiscrete("lens_mapping", invalid, false);
  for (std::string_view key : {"optical_plane", "lens_mapping"}) {
    testDiscrete(key, false, false);
    testDiscrete(key, std::string("0"), false);
  }

  // Every generated field must survive the same renderer-only serialization
  // used by live previews and committed login appearance, including zero/false.
  ShellConfig shell;
  shell.animation.style = MotionStyle::Custom;
  shell.animation.enabled = false;
  shell.animation.curveX1 = 0;
  shell.animation.curveY1 = -2;
  shell.animation.curveX2 = 1;
  shell.animation.curveY2 = 2;
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) shell.design.member = low;
#include "ui/style_tokens.def"
#undef STYLE_TOKEN
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) shell.material.key = low;
#include "material/fields.def"
#undef MATERIAL_FIELD
#define CONTROL_ENUM(member, type, initial, label, group) \
  shell.controls.member = *Style::parseControl<Style::type>(Style::controlKeys<Style::type>().back());
#define CONTROL_NUMBER(member, initial, low, high, step, label, group) shell.controls.member = low;
#define CONTROL_BOOL(member, initial, label, group) shell.controls.member = false;
#include "ui/control_settings.def"
#undef CONTROL_ENUM
#undef CONTROL_NUMBER
#undef CONTROL_BOOL
  const auto snapshot = json::parse(greeter::detail::appearanceSnapshotJson(shell, Palette{}));
  check(snapshot.at("animation").at("style") == "custom" && !snapshot.at("animation").at("enabled").get<bool>(),
      "Motion style/None missing from appearance transport");
  check(snapshot.at("animation").at("curve_x1") == 0 && snapshot.at("animation").at("curve_y1") == -2
      && snapshot.at("animation").at("curve_x2") == 1 && snapshot.at("animation").at("curve_y2") == 2,
      "Custom curve explicit zero and signed bounds changed in appearance transport");
#define STYLE_TOKEN(type, member, key, initial, low, high, step, label, group) \
  check(snapshot.at("design").at(key).get<type>() == low, "Design metric missing from appearance JSON: " key);
#include "ui/style_tokens.def"
#undef STYLE_TOKEN
#define MATERIAL_FIELD(member, key, initial, low, high, step, label, group) \
  check(snapshot.at("material").at(#key).get<float>() == low, "Material field missing from appearance JSON: " #key);
#include "material/fields.def"
#undef MATERIAL_FIELD
#define CONTROL_ENUM(member, type, initial, label, group) \
  check(snapshot.at("controls").at(#member).get<std::string>() == Style::controlName(shell.controls.member), \
      "Control choice missing from appearance JSON: " #member);
#define CONTROL_NUMBER(member, initial, low, high, step, label, group) \
  check(snapshot.at("controls").at(#member).get<float>() == low, "Control scalar missing from appearance JSON: " #member);
#define CONTROL_BOOL(member, initial, label, group) \
  check(snapshot.at("controls").at(#member) == false, "False control value missing from appearance JSON: " #member);
#include "ui/control_settings.def"
#undef CONTROL_ENUM
#undef CONTROL_NUMBER
#undef CONTROL_BOOL
}

DesktopWidgetState lockClock(std::string id, float cx) {
  DesktopWidgetState widget;
  widget.id = std::move(id);
  widget.type = "clock";
  widget.outputName = "DP-1";
  widget.cx = cx;
  widget.cy = 220.0F;
  widget.placementWidth = 1920.0F;
  widget.placementHeight = 1080.0F;
  widget.boxWidth = 280.0F;
  widget.boxHeight = 120.0F;
  widget.rotationRad = 0.25F;
  widget.flipX = true;
  widget.settings.emplace("format", std::string("{:%H:%M}"));
  widget.settings.emplace("center_text", false);
  widget.settings.emplace("background_opacity", 0.0);
  return widget;
}

void lockWidgetAppearanceOwnershipAndTransport() {
  Config config;
  config.lockscreenWidgets.enabled = true; // Runtime policy must not be serialized.
  config.lockscreenWidgets.grid.visible = false;
  config.lockscreenWidgets.widgets.push_back(lockClock("clock@DP-1", 420.0F));

  auto volume = lockClock("volume@DP-1", 1500.0F);
  volume.type = "volume";
  volume.settings = {{"fill_color", std::string("primary")}, {"device", std::string("input")},
      {"scroll_step", std::int64_t{25}}};
  config.lockscreenWidgets.widgets.push_back(std::move(volume));

  auto login = lockClock("login@DP-1", 960.0F);
  login.type = "login_box";
  login.settings = {{"layout", std::string("compact")}, {"background_opacity", 0.75},
      {"show_session_buttons", false}, {"show_login_button", false}};
  config.lockscreenWidgets.widgets.push_back(std::move(login));

  auto command = lockClock("button@DP-1", 100.0F);
  command.type = "button";
  command.settings = {{"command", std::string("shutdown now")}};
  config.lockscreenWidgets.widgets.push_back(std::move(command));
  auto plugin = lockClock("plugin@DP-1", 200.0F);
  plugin.type = "vendor/plugin:lock";
  config.lockscreenWidgets.widgets.push_back(std::move(plugin));
  auto invalid = lockClock("invalid@DP-1", std::numeric_limits<float>::infinity());
  config.lockscreenWidgets.widgets.push_back(std::move(invalid));

  const auto snapshot = json::parse(greeter::detail::appearanceSnapshotJson(config, Palette{}));
  const auto& layout = snapshot.at("lock_widgets");
  check(layout.at("version") == 1 && layout.at("widgets").size() == 3,
      "Appearance publisher did not retain only bounded first-party lock widgets");
  check(!layout.contains("enabled") && !layout.contains("grid"), "Lock runtime policy escaped appearance transport");
  check(layout["widgets"][0]["cx"] == 420.0 && layout["widgets"][0]["settings"]["background_opacity"] == 0.0,
      "Lock widget geometry or explicit presentation zero changed during serialization");
  check(!layout["widgets"][1]["settings"].contains("device")
      && !layout["widgets"][1]["settings"].contains("scroll_step"),
      "Device selection or action policy escaped lock appearance transport");
  check(!layout["widgets"][2]["settings"].contains("show_session_buttons")
      && !layout["widgets"][2]["settings"].contains("show_login_button"),
      "Authentication/session controls escaped lock appearance transport");

  check(noctalia::profile::owns({"lockscreen_widgets", "schema_version"})
      && noctalia::profile::owns({"lockscreen_widgets", "grid"})
      && noctalia::profile::owns({"lockscreen_widgets", "widget"})
      && !noctalia::profile::owns({"lockscreen_widgets", "enabled"}),
      "Lock appearance profile ownership includes policy or omits presentation");
  const toml::table source{{"lockscreen_widgets", toml::table{{"enabled", true}, {"schema_version", 2},
      {"grid", toml::table{{"visible", false}}}, {"widget", toml::array{toml::table{{"id", "clock"}}}}}}};
  const auto owned = noctalia::profile::subset(source);
  const auto* ownedLock = owned["lockscreen_widgets"].as_table();
  check(ownedLock && !ownedLock->contains("enabled") && ownedLock->contains("schema_version")
      && ownedLock->contains("grid") && ownedLock->contains("widget"),
      "Profile subset did not isolate lock presentation ownership");
}

void writeFile(const fs::path& path, std::string_view content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::trunc);
  out << content;
  out.close();
  check(out.good(), "Cannot write isolated test file");
}

json readJson(const fs::path& path) {
  std::ifstream in(path);
  return json::parse(in);
}

struct Fixture {
  fs::path root;
  fs::path settings;
  std::vector<std::pair<std::string, std::optional<std::string>>> environment;

  Fixture() {
    std::string pattern = (fs::temp_directory_path() / "noctalia-profile-test-XXXXXX").string();
    const char* created = ::mkdtemp(pattern.data());
    check(created != nullptr, "Cannot create private test directory");
    root = created;
    for (const auto& [name, child] : std::vector<std::pair<std::string, std::string>>{
             {"NOCTALIA_CONFIG_HOME", "config"}, {"NOCTALIA_STATE_HOME", "state"},
             {"NOCTALIA_DATA_HOME", "data"}, {"XDG_DATA_HOME", "data-xdg"}, {"XDG_CACHE_HOME", "cache"},
             {"XDG_RUNTIME_DIR", "runtime"}}) {
      const char* previous = std::getenv(name.c_str());
      environment.emplace_back(name, previous ? std::optional<std::string>(previous) : std::nullopt);
      fs::create_directories(root / child / "noctalia");
      if (name == "XDG_RUNTIME_DIR")
        check(::chmod((root / child).c_str(), 0700) == 0, "Cannot make isolated runtime directory private");
      check(::setenv(name.c_str(), (root / child).c_str(), 1) == 0, "Cannot isolate test environment");
    }
    settings = root / "state/noctalia/settings.toml";
    writeFile(root / "config/noctalia/config.toml", "\n");
    disk(12.0, "Noctalia", false);
  }

  ~Fixture() {
    for (const auto& [name, value] : environment) {
      if (value) ::setenv(name.c_str(), value->c_str(), 1);
      else ::unsetenv(name.c_str());
    }
    std::error_code error;
    fs::remove_all(root, error);
  }

  void disk(double radius, std::string_view palette, bool black) const {
    writeFile(settings, "[shell.design]\nradius_xl = " + std::to_string(radius)
        + "\n[theme]\nbuiltin = \"" + std::string(palette)
        + "\"\npure_black_dark = " + (black ? "true" : "false") + "\n");
  }

  // A directory at the atomic temporary-file path rejects open(O_TRUNC)
  // deterministically, including when the tests run as root. Settings stay readable.
  void failWrites() const { fs::create_directory(settings.string() + ".tmp"); }
  void allowWrites() const { fs::remove(settings.string() + ".tmp"); }

  json request(ConfigService& service, std::string_view action, json payload) const {
    if (!payload.contains("expected")) payload["expected"] = json::array();
    const auto path = root / "request.json";
    writeFile(path, payload.dump());
    check(::chmod(path.c_str(), 0600) == 0, "Cannot make request private");
    return json::parse(service.profileRequest(std::string(action) + " " + path.string()));
  }
};

void deferredSettingsMutationLifetime() {
  Fixture fixture;
  ConfigService service;
  const auto config = [&] { return service.profileRequest("snapshot"); };
  const auto drain = [] {
    // Mutations may enqueue a subsequent layout refresh; both are real queue
    // passes. A remaining queue is a lifecycle regression, not an infinite wait.
    for (int pass = 0; pass < 8; ++pass) {
      auto callbacks = DeferredCall::takePending();
      if (callbacks.empty()) return;
      for (auto& callback : callbacks) callback();
    }
    check(DeferredCall::takePending().empty(), "Settings callbacks failed to settle");
  };
  drain();
  auto window = std::make_unique<SettingsWindow>();
  SettingsWindowMutationTestAccess::attach(*window, service);
  SettingsWindowMutationTestAccess::setRadius(*window, 24);
  check(service.config().shell.design.radiusXl != 24, "Setting mutation ran before its deferred turn");
  drain();
  check(service.config().shell.design.radiusXl == 24, "Current-window deferred write was dropped");
  SettingsWindowMutationTestAccess::setInvalidRadius(*window);
  drain();
  check(SettingsWindowMutationTestAccess::errorStatus(*window), "Current-window mutation lost its error status");
  check(service.config().shell.design.radiusXl == 24, "Invalid current-window write replaced the accepted draft");
  const auto baseline = config();
  const auto bar = service.config().bars.front().name;
  SettingsWindowMutationTestAccess::queueAllOrdinaryActions(*window, bar);
  auto callbacks = DeferredCall::takePending();
  check(callbacks.size() == 13, "Fixture did not queue all ordinary settings actions");
  window.reset();
  for (auto& callback : callbacks) callback();
  drain();
  check(config() == baseline, "Destroyed settings owner applied a queued mutation");

  window = std::make_unique<SettingsWindow>();
  SettingsWindowMutationTestAccess::attach(*window, service);
  SettingsWindowMutationTestAccess::queueAllOrdinaryActions(*window, bar);
  callbacks = DeferredCall::takePending();
  // Actual teardown advances the window generation. Reattach the same retained
  // controller to represent a fresh open, without constructing Wayland objects.
  SettingsWindowMutationTestAccess::closeWindow(*window);
  SettingsWindowMutationTestAccess::attach(*window, service);
  for (auto& callback : callbacks) callback();
  drain();
  check(config() == baseline && SettingsWindowMutationTestAccess::freshStatus(*window),
      "Closed-window callback modified the reopened window or config");
  SettingsWindowMutationTestAccess::setRadius(*window, 28);
  drain();
  check(service.config().shell.design.radiusXl == 28, "Reopened window cannot apply its own fresh mutation");
  window.reset();
  drain();
}

void settingsCurveAndGroupedReset() {
  Fixture fixture;
  ConfigService service;
  std::string text;
  std::vector<std::string> selected;
  std::unordered_map<std::string, std::unordered_set<std::string>> groups;
  bool editing = false;
  settings::SettingsContentContext context{
      .config = service.config(), .configService = &service,
      .editingWidgetName = text, .editingCapsuleGroupId = text, .selectedLaneWidgets = selected,
      .pendingDeleteWidgetName = text, .pendingDeleteWidgetSettingPath = text, .renamingWidgetName = text,
      .pendingGestureKey = text, .pendingGestureVerb = text, .actionsExpandedFor = text,
      .expandedGroupsByPage = groups,
      .setInteractiveEdit = [&](bool active) { editing = active; },
      .requestRebuild = [] {},
      .setOverride = [&](auto path, auto value) { check(service.setOverride(path, value), service.lastMutationError()); },
      .setOverrides = [&](auto values) { check(service.setOverrides(std::move(values)), service.lastMutationError()); },
      .clearOverrides = [&](const auto& paths) {
        bool changed = false;
        check(service.clearOverrides(paths, &changed), service.lastMutationError());
      },
      .isResetConfirmationPending = [](const auto&) { return true; },
  };
  settings::SettingsControlFactory factory(context);
  // A real slider drag reaches the config before pointer release. Setting a
  // controlled value outside a drag must remain callback-free for persistence.
  {
    auto sliderRow = factory.makeSlider(1.0, 0.1, 4.0, 0.1, {"shell", "animation", "speed"}, false);
    const auto findSlider = [&](const auto& self, Node& node) -> Slider* {
      if (auto* slider = dynamic_cast<Slider*>(&node)) return slider;
      for (const auto& child : node.children()) if (auto* slider = self(self, *child)) return slider;
      return nullptr;
    };
    auto* slider = findSlider(findSlider, *sliderRow);
    check(slider != nullptr, "Factory slider fixture is missing its control");
    slider->setValue(2.0);
    check(service.config().shell.animation.speed == 1.0F, "Controlled slider assignment persisted a setting");
    InputArea* area = nullptr;
    for (const auto& child : slider->children()) if (auto* inputArea = dynamic_cast<InputArea*>(child.get())) area = inputArea;
    check(area != nullptr, "Slider input area missing");
    area->setFrameSize(200, 30);
    area->dispatchPress(10, 15, BTN_LEFT, true);
    slider->setValue(3.0);
    check(slider->dragging() && service.config().shell.animation.speed == 3.0F,
        "Slider did not preview while the pointer is held");
    area->dispatchPress(10, 15, BTN_LEFT, false);
    check(service.setOverride({"shell", "animation", "speed"}, 1.0), service.lastMutationError());
  }

  settings::CurveSetting curve;
  curve.stylePath = {"shell", "animation", "style"};
  curve.initialStyle = "native";
  constexpr const char* keys[]{"curve_x1", "curve_y1", "curve_x2", "curve_y2"};
  for (std::size_t i = 0; i < curve.paths.size(); ++i) curve.paths[i] = {"shell", "animation", keys[i]};
  auto control = factory.makeCurve(curve);
  auto* editor = dynamic_cast<BezierEditor*>(control.get());
  check(editor != nullptr, "Curve setting did not create the production BezierEditor");
  const auto drainCurvePreview = [] {
    for (int pass = 0; pass < 4; ++pass) {
      auto callbacks = DeferredCall::takePending();
      if (callbacks.empty()) return;
      for (auto& callback : callbacks) callback();
    }
    check(DeferredCall::takePending().empty(), "Curve preview callbacks failed to settle");
  };
  auto* input = editor->inputArea();
  input->dispatchFocusGain();
  input->dispatchKey(XKB_KEY_Right, 0, 0, true);
  input->dispatchKey(XKB_KEY_Right, 0, 0, true);
  input->dispatchKey(XKB_KEY_Right, 0, 0, true);
  check(service.config().shell.animation.curveX1 == curve.value[0],
      "Curve pointer burst was written before its coalesced preview callback");
  drainCurvePreview();
  check(editing && service.config().shell.animation.style == MotionStyle::Custom,
      "Keyboard curve preview did not retain the edit or select Custom on the next UI turn");
  check(service.config().shell.animation.curveX1 > curve.value[0], "Keyboard curve preview was not applied live");
  input->dispatchKey(XKB_KEY_Escape, 0, 0, true);
  drainCurvePreview();
  check(!editing && input->focused() && editor->curve() == curve.value,
      "Escape did not end the edit while retaining focus and original handles");
  check(service.config().shell.animation.style == MotionStyle::Native,
      "Canceling the handle edit did not restore its original motion mode");
  input->dispatchKey(XKB_KEY_Right, 0, 0, true);
  input->dispatchKey(XKB_KEY_Right, 0, 0, false);
  drainCurvePreview();
  check(!editing && service.config().shell.animation.style == MotionStyle::Custom,
      "Keyboard release did not finish the live edit");

  // Exercise the real plugin page, including graph metadata, atomic writes,
  // numeric fallback controls and a non-Custom activation choice.
  {
    scripting::PluginManifest manifest;
    manifest.id = "test/curve";
    scripting::ManifestField policy;
    policy.key = "policy";
    policy.labelKey = "policy";
    policy.type = scripting::ManifestFieldType::Select;
    policy.stringDefault = "native";
    policy.options = {{"native", "native"}, {"configured", "configured"}};
    manifest.settings.push_back(policy);
    const std::array<std::string, 4> coordinates{"x1", "y1", "x2", "y2"};
    for (std::size_t i = 0; i < coordinates.size(); ++i) {
      scripting::ManifestField field;
      field.key = coordinates[i];
      field.labelKey = field.key;
      field.type = scripting::ManifestFieldType::Double;
      field.numberDefault = curve.value[i];
      field.minValue = i % 2 == 0 ? 0 : -2;
      field.maxValue = i % 2 == 0 ? 1 : 2;
      field.step = .01;
      if (i == 0) field.curve = scripting::ManifestCurveGroup{
          .keys = coordinates, .labelKey = "Window curve", .activationKey = "policy", .activationValue = "configured"};
      manifest.settings.push_back(field);
    }
    std::size_t batches = 0;
    auto pluginContext = context;
    pluginContext.setOverrides = [&](auto values) {
      ++batches;
      check(values.size() == 5, "Plugin graph did not submit all coordinates and policy in one batch");
      check(service.setOverrides(std::move(values)), service.lastMutationError());
    };
    settings::SettingsControlFactory pluginFactory(pluginContext);
    Flex page;
    check(settings::buildPluginSettingsEditor(page, service.config(), pluginFactory, manifest.id, manifest, true, 1.0F, {}),
        "Plugin curve page was not rendered");
    BezierEditor* graph = nullptr;
    std::size_t graphs = 0, numbers = 0;
    const auto inspect = [&](const auto& self, Node& node) -> void {
      if (auto* candidate = dynamic_cast<BezierEditor*>(&node)) { graph = candidate; ++graphs; }
      if (dynamic_cast<Slider*>(&node)) ++numbers;
      for (const auto& child : node.children()) self(self, *child);
    };
    inspect(inspect, page);
    check(graphs == 1 && numbers == 4 && graph && batches == 0,
        "Plugin curve must retain one graph and four callback-free numeric controls");
    auto* graphInput = graph->inputArea();
    graphInput->dispatchFocusGain();
    graphInput->dispatchKey(XKB_KEY_Right, 0, 0, true);
    graphInput->dispatchKey(XKB_KEY_Right, 0, 0, true);
    graphInput->dispatchKey(XKB_KEY_Right, 0, 0, true);
    drainCurvePreview();
    check(editing && batches == 1 && std::get<std::string>(service.config().plugins.pluginSettings.at(manifest.id).at("policy")) == "configured",
        "Graph burst was not coalesced into one live-preview batch with the declared plugin policy");
    check(std::get<double>(service.config().plugins.pluginSettings.at(manifest.id).at("x1")) > curve.value[0],
        "Plugin graph coordinate did not preview immediately");
    graphInput->dispatchKey(XKB_KEY_Escape, 0, 0, true);
    drainCurvePreview();
    check(!editing && batches == 2 && graph->curve() == curve.value &&
        std::get<std::string>(service.config().plugins.pluginSettings.at(manifest.id).at("policy")) == "native",
        "Plugin graph Escape failed to restore coordinates and original activation policy");
  }

  auto clickReset = [&](const settings::SettingEntry& entry, std::unique_ptr<Node> value) {
    Flex row;
    factory.makeRow(row, entry, std::move(value));
    const auto find = [&](const auto& self, Node& node) -> Button* {
      if (auto* button = dynamic_cast<Button*>(&node)) return button;
      for (const auto& child : node.children()) if (auto* button = self(self, *child)) return button;
      return nullptr;
    };
    auto* reset = find(find, row);
    check(reset != nullptr, "Overridden setting did not expose a Reset button: " + entry.title);
    reset->inputArea()->setFrameSize(20, 20);
    reset->inputArea()->dispatchPress(10, 10, BTN_LEFT, true);
    reset->inputArea()->dispatchPress(10, 10, BTN_LEFT, false);
  };
  settings::SettingEntry entry;
  entry.title = "Curve fixture";
  entry.path = curve.paths[0];
  entry.control = curve;
  check(service.hasOverride(curve.stylePath), "Live Custom curve has no stored mode override");
  check(service.hasEffectiveOverride(curve.stylePath), "Live Custom curve mode is incorrectly considered redundant");
  check(settings::settingEntryHasEffectiveOverride(entry, service), "Curve row did not include its effective mode override");
  clickReset(entry, std::move(control));
  check(service.config().shell.animation.style == MotionStyle::Native
      && service.config().shell.animation.curveX1 == ShellConfig::AnimationConfig{}.curveX1,
      "Grouped curve Reset left its implicit Custom mode or a coordinate override behind");

  // The actual named-shape row owns nine ordinary bar properties. Its Reset
  // must restore the prior effective bar even when only a linked field differs.
  check(!service.config().bars.empty(), "Fixture has no default bar");
  const BarConfig baseline = service.config().bars.front();
  const std::vector<std::string> basePath{"bar", baseline.name};
  entry.title = "Bar shape fixture";
  entry.path = basePath;
  entry.path.emplace_back("section_backgrounds");
  auto shape = settings::barPresentationSetting(baseline, basePath);
  // The default bar is already Notch. Full keeps section_backgrounds false
  // while changing linked margins/corners, so this exercises linked-only Reset.
  check(service.setOverrides(shape.groupedCommit("full", entry.path)), service.lastMutationError());
  check(service.config().bars.front() != baseline, "Bar-shape fixture did not change any effective geometry");
  entry.control = shape;
  clickReset(entry, std::make_unique<Node>());
  check(service.config().bars.front() == baseline, "Grouped bar-shape Reset left a linked geometry override behind");
}

json status(ConfigService& service) {
  auto result = json::parse(service.profileRequest("status"));
  check(result.at("ok").get<bool>(), "Status request failed");
  return result;
}

json snapshot(ConfigService& service, std::string_view action = "snapshot") {
  auto result = json::parse(service.profileRequest(std::string(action)));
  check(result.at("ok").get<bool>(), "Config snapshot failed");
  return result.at("config");
}

json guard(ConfigService& service) {
  const auto current = status(service);
  return {{"session", current.at("session")}, {"generation", current.at("generation")}};
}

void radius(ConfigService& service, double value) {
  check(service.setOverride({"shell", "design", "radius_xl"}, value), service.lastMutationError());
  check(snapshot(service)["shell"]["design"]["radius_xl"] == value, "Preview not immediately effective");
}

double diskRadius(const Fixture& fixture) {
  return toml::parse_file(fixture.settings.string())["shell"]["design"]["radius_xl"].value<double>().value();
}

void assertPalette(ConfigService& service, std::string_view palette, bool black) {
  const auto config = snapshot(service);
  check(config["theme"]["builtin"] == palette, "External palette was lost");
  check(config["theme"]["pure_black_dark"] == black, "Independent setting was lost");
}

void designTokenMutationValidation() {
  Fixture fixture;
  ConfigService service;
  radius(service, 24.0);
  const auto before = snapshot(service);
  const auto generation = status(service).at("generation");
  const auto reject = [&](std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> changes) {
    check(!service.setOverrides(std::move(changes)), "Malformed design token transaction was accepted");
    check(!service.lastMutationError().empty(), "Malformed design token lost its error");
    check(snapshot(service) == before && status(service).at("generation") == generation,
        "Rejected design token changed the draft or preview generation");
    check(diskRadius(fixture) == 12.0, "Rejected design token changed the committed file");
  };
  reject({{{"shell", "design", "radius_xl"}, std::string("invalid")}});
  reject({{{"shell", "design", "radius_xl"}, false}});
  reject({{{"shell", "design", "radius_xl"}, std::numeric_limits<double>::infinity()}});
  reject({{{"shell", "design", "radius_xl"}, 35.0},
          {{"shell", "design", "space_sm"}, std::string("invalid")}});
  radius(service, 0.0);
  check(service.config().shell.design.radiusXl == 0, "Explicit zero design token was rejected");
  auto entries = settings::buildSettingsRegistry(service.config(), nullptr, nullptr);
  const auto caret = std::ranges::find_if(entries, [](const auto& row) {
    return row.path == std::vector<std::string>{"shell", "caret", "width_px"};
  });
  check(caret != entries.end(), "Caret width row missing from actual settings registry");
  check(caret->advanced, "Caret geometry leaked into the Basic rice view");
  const auto& slider = std::get<settings::SliderSetting>(caret->control);
  check(slider.step == 0.25 && slider.minValue == 1 && slider.maxValue == 8,
      "Caret width slider lost its explicit range or fractional step");
  const auto screenCorners = std::ranges::find_if(entries, [](const auto& row) {
    return row.path == std::vector<std::string>{"shell", "screen_corners", "size"};
  });
  const auto desktopFrame = std::ranges::find_if(entries, [](const auto& row) {
    return row.path == std::vector<std::string>{"shell", "desktop_frame", "enabled"};
  });
  check(screenCorners != entries.end() && !screenCorners->advanced,
      "Rounded screen corners disappeared from the Basic rice view");
  check(desktopFrame != entries.end() && desktopFrame->advanced,
      "Decorative desktop frame leaked into Basic as a rounded-corner substitute");
}

void cornerPowerProfileAndInputFocus() {
  Fixture fixture;
  ConfigService service;
  const std::vector<std::string> path{"shell", "design", "corner_power"};
  const auto baselineMetrics = Style::metrics();
  struct RestoreMetrics {
    Style::Metrics metrics;
    ~RestoreMetrics() { Style::setMetrics(metrics); }
  } restore{baselineMetrics};

  Input input;
  input.setValue("focused åä corner");
  input.selectAll();
  input.inputArea()->dispatchFocusGain();
  const auto before = input.textInputState();
  auto changedMetrics = baselineMetrics;
  changedMetrics.cornerPower = 4.0F;
  Style::setMetrics(changedMetrics);
  const auto after = input.textInputState();
  check(input.inputArea()->focused() && after.surroundingText == before.surroundingText
          && after.cursor == before.cursor && after.anchor == before.anchor,
      "Live corner-power refresh changed native Input focus, caret or selection");

  const auto original = snapshot(service)["shell"]["design"]["corner_power"];
  const auto generation = status(service).at("generation");
  for (const ConfigOverrideValue invalid : {ConfigOverrideValue{std::string("4")}, ConfigOverrideValue{false},
                                           ConfigOverrideValue{std::numeric_limits<double>::infinity()}}) {
    check(!service.setOverride(path, invalid), "Invalid corner power was accepted");
    check(snapshot(service)["shell"]["design"]["corner_power"] == original
            && status(service).at("generation") == generation,
        "Rejected corner power changed the profile draft");
  }

  for (const auto [requested, clamped] : {std::pair{1.9, 2.0}, std::pair{10.1, 10.0}}) {
    check(service.setOverride(path, requested), service.lastMutationError());
    check(snapshot(service)["shell"]["design"]["corner_power"] == clamped,
        "Out-of-range corner power did not use the design-token clamp");
    check(snapshot(service, "committed")["shell"]["design"]["corner_power"] == original,
        "Clamped corner-power preview leaked into committed appearance");
    service.cancelProfilePreview();
    check(snapshot(service)["shell"]["design"]["corner_power"] == original,
        "Cancel did not restore the baseline after a clamped corner-power preview");
  }

  check(service.setOverride(path, 4.0), service.lastMutationError());
  check(snapshot(service)["shell"]["design"]["corner_power"] == 4.0
          && snapshot(service, "committed")["shell"]["design"]["corner_power"] == original,
      "Corner-power preview leaked into committed appearance");
  service.cancelProfilePreview();
  check(snapshot(service)["shell"]["design"]["corner_power"] == original,
      "Corner-power Cancel did not restore the baseline");

  check(service.setOverride(path, 10.0), service.lastMutationError());
  check(service.commitProfilePreview(), service.lastMutationError());
  ConfigService reopened;
  check(snapshot(reopened)["shell"]["design"]["corner_power"] == 10.0,
      "Corner-power Save did not survive reopen");
  const auto stored = toml::parse_file(fixture.settings.string())["shell"]["design"]["corner_power"].value<double>();
  check(stored && *stored == 10.0, "Corner-power Save was not durable");
}

void ownedAnimationRouting() {
  const auto drain = [] {
    for (int pass = 0; pass < 8; ++pass) {
      auto callbacks = DeferredCall::takePending();
      if (callbacks.empty()) return;
      for (auto& callback : callbacks) callback();
    }
    check(DeferredCall::takePending().empty(), "Owned setting callbacks failed to settle");
  };
  Fixture fixture;
  const auto pluginRoot = fixture.root / "owned-plugins";
  writeFile(pluginRoot / "motion/plugin.toml", R"(
id = "test/motion"
name = "Motion owner"
version = "1.0.0"
plugin_api = 23
[[settings_ownership]]
path = ["shell", "animation", "enabled"]
when = "integration"
setting = "animations"
[[settings_ownership]]
path = ["shell", "animation", "speed"]
when = "integration"
setting = "speed"
[[setting]]
key = "integration"
label_key = "integration"
type = "bool"
default = true
[[setting]]
key = "animations"
label_key = "animations"
type = "bool"
default = true
[[setting]]
key = "speed"
label_key = "speed"
type = "double"
default = 1.0
min = 0.0
max = 4.0
)");
  writeFile(fixture.root / "config/noctalia/config.toml", "[plugins]\nenabled = [\"test/motion\"]\n");
  auto& plugins = scripting::PluginRegistry::instance();
  plugins.setSources({pluginRoot});
  plugins.setEnabledFilter(std::unordered_set<std::string>{"test/motion"});
  plugins.scan();
  struct ResetRegistry {
    ~ResetRegistry() {
      auto& registry = scripting::PluginRegistry::instance();
      registry.setSources({}); registry.setEnabledFilter(std::nullopt); registry.scan();
    }
  } resetRegistry;
  ConfigService service;
  const std::vector<std::string> nativePath{"shell", "animation", "enabled"};
  const std::vector<std::string> ownerPath{"plugin_settings", "test/motion", "animations"};
  const std::vector<std::string> nativeSpeedPath{"shell", "animation", "speed"};
  const std::vector<std::string> ownerSpeedPath{"plugin_settings", "test/motion", "speed"};
  const auto resolved = settings::pluginSettingRoute(service.config(), nativePath);
  check(resolved && resolved->path == ownerPath && resolved->value, "Native boolean did not resolve its active owner");
  {
    auto entries = settings::buildSettingsRegistry(service.config(), nullptr, nullptr);
    const auto speedEntry = std::ranges::find_if(entries, [&](const auto& row) {
      return row.path == nativeSpeedPath;
    });
    check(speedEntry != entries.end(), "Owned animation speed slider missing");
    const auto& speed = std::get<settings::SliderSetting>(speedEntry->control);

    auto offWrites = speed.groupedCommit(0.0);
    check(settings::routePluginSettingWrites(service.config(), offWrites)
            && offWrites == std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
                {ownerPath, false}},
        "Speed 0 did not route through the compositor animation owner as disabled");

    auto onWrites = speed.groupedCommit(1.5);
    check(settings::routePluginSettingWrites(service.config(), onWrites)
            && onWrites == std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
                {ownerSpeedPath, 1.5}, {ownerPath, true}},
        "Positive speed did not route speed plus enabled through the compositor owner");
  }
  const auto motion = [&] {
    auto entries = settings::buildSettingsRegistry(service.config(), nullptr, nullptr);
    settings::applyPluginSettingRoutes(service.config(), entries);
    const auto entry = std::ranges::find_if(entries, [](const auto& row) {
      return row.path == std::vector<std::string>{"shell", "animation", "style"};
    });
    check(entry != entries.end(), "Motion selector missing");
    return std::get<settings::SelectSetting>(entry->control);
  };
  auto window = std::make_unique<SettingsWindow>();
  SettingsWindowMutationTestAccess::attach(*window, service);
  SettingsWindowMutationTestAccess::setBatch(*window, {{nativeSpeedPath, 2.25}});
  drain();
  check(service.config().shell.animation.speed == 1.0F
          && std::get<double>(service.config().plugins.pluginSettings.at("test/motion").at("speed")) == 2.25,
      "Native numeric animation write did not route exclusively to the shared owner");
  check(service.setOverride(nativeSpeedPath, 2.25), service.lastMutationError());
  check(std::get<double>(service.config().plugins.pluginSettings.at("test/motion").at("speed")) == 2.25,
      "Accepted native numeric mirror fed back into or changed the owner");
  auto choice = motion();
  SettingsWindowMutationTestAccess::setBatch(*window, choice.groupedCommit("none", {"shell", "animation", "style"}));
  drain();
  check(!settings::pluginSettingRoute(service.config(), nativePath)->value && service.config().shell.animation.enabled,
      "None must change the requested owner without racing the acknowledged native mirror");
  check(motion().selectedValue == "none" && motion().linkedPath == ownerPath,
      "Pending None state/reset path did not follow the authoritative plugin control");
  // The compositor's accepted state updates the native mirror through its
  // existing IPC path, which deliberately bypasses the settings UI adapter.
  check(service.setOverride(nativePath, false), service.lastMutationError());
  choice = motion();
  SettingsWindowMutationTestAccess::setBatch(*window, choice.groupedCommit("linear", {"shell", "animation", "style"}));
  drain();
  check(settings::pluginSettingRoute(service.config(), nativePath)->value && !service.config().shell.animation.enabled &&
      service.config().shell.animation.style == MotionStyle::Linear && motion().selectedValue == "linear",
      "Re-enable did not show the pending owner and native style together");
  check(service.setOverride(nativePath, true), service.lastMutationError());
  check(service.profilePreviewDirty(), "Accepted native appearance must remain transactional");
  const auto cancel = fixture.request(service, "cancel", guard(service));
  check(cancel.at("ok").get<bool>(), cancel.value("error", "Native Cancel request failed"));
  check(service.config().shell.animation.style == MotionStyle::Native,
      "Native Cancel did not restore appearance after owned changes");
  // Reject a backend operation by mirroring its accepted value. The requested
  // UI must follow that correction rather than reasserting a second authority.
  check(service.setOverride(ownerPath, false), service.lastMutationError());
  check(motion().selectedValue == "none", "Rejected owner mirror was not reflected in the selector");
  SettingsWindowMutationTestAccess::clearBatch(*window, {{"shell", "animation", "style"}, nativePath});
  drain();
  check(settings::pluginSettingRoute(service.config(), nativePath)->value && motion().selectedValue == "native",
      "Grouped reset did not restore the owner default and native curve style");
  std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> conflicting{{nativePath, false}, {ownerPath, true}};
  check(!settings::routePluginSettingWrites(service.config(), conflicting) && conflicting.front().first == nativePath,
      "Conflicting aliases must reject the complete batch without rewriting input");
  SettingsWindowMutationTestAccess::setBatch(*window, {{nativePath, std::string("false")}});
  drain();
  check(SettingsWindowMutationTestAccess::errorStatus(*window) && settings::pluginSettingRoute(service.config(), nativePath)->value,
      "Invalid routed boolean was accepted or partially written");
  SettingsWindowMutationTestAccess::setBatch(*window, {{nativeSpeedPath, std::string("2.5")}});
  drain();
  check(SettingsWindowMutationTestAccess::errorStatus(*window)
          && std::get<double>(service.config().plugins.pluginSettings.at("test/motion").at("speed")) == 2.25,
      "Invalid routed numeric value was coerced or partially written");
  check(service.setOverride({"plugin_settings", "test/motion", "integration"}, false), service.lastMutationError());
  check(!settings::pluginSettingRoute(service.config(), nativePath), "Disabled integration still redirected native settings");
  window.reset();
  drain();
}

void emptyDesktopWidgetProfileRoundtrip() {
  Fixture fixture;
  writeFile(fixture.root / "config/noctalia/config.toml",
      "[desktop_widgets]\nenabled = true\nwidget_order = [\"base-clock\"]\n"
      "[desktop_widgets.widget.base-clock]\ntype = \"clock\"\ncx = 100\ncy = 100\n");
  ConfigService service;
  check(service.config().desktopWidgets.widgets.size()==1,"Populated base fixture was not loaded");
  const auto emptyApply=[&](const json& value) {
    auto request=guard(service);
    request["entries"]=json::array({{{"path",{"desktop_widgets"}},{"value",value}}});
    request["expected"]=json::array({{{"path",{"desktop_widgets"}},{"value",snapshot(service)["desktop_widgets"]}}});
    const auto result=fixture.request(service,"apply",request);
    check(result.at("ok").get<bool>(),result.dump());
  };
  auto empty=snapshot(service)["desktop_widgets"];
  empty["widget"]=json::object();empty["widget_order"]=json::array();
  emptyApply(empty);
  check(service.config().desktopWidgets.widgets.empty(),"Explicit empty profile retained base widgets during Preview");
  check(service.committedConfig()->desktopWidgets.widgets.size()==1,"Preview changed committed base widgets");
  const auto exported=snapshot(service)["desktop_widgets"];
  service.cancelProfilePreview();
  check(service.config().desktopWidgets.widgets.size()==1,"Cancel did not restore base widgets");
  // Re-import the exported effective empty profile over the populated base.
  emptyApply(exported);
  check(service.config().desktopWidgets.widgets.empty(),"Export/import of empty profile resurrected base widgets");
  check(service.commitProfilePreview(),service.lastMutationError());
  check(service.config().desktopWidgets.widgets.empty(),"Save resurrected base widgets");
  { ConfigService reloaded;
    check(reloaded.config().desktopWidgets.widgets.empty(),"Reload after Save resurrected base widgets");
  }
  const auto disk=toml::parse_file(fixture.settings.string());
  check(disk["desktop_widgets"]["widget"].is_table(),"Saved clear lost its explicit empty widget table");
  check(disk["desktop_widgets"]["widget_order"].is_array(),"Saved clear lost its explicit empty widget order");
}

void widgetFlipResetRoundtrip() {
  for (const std::string section : {"desktop_widgets", "lockscreen_widgets"}) {
    Fixture fixture;
    writeFile(fixture.root / "config/noctalia/config.toml",
        "[" + section + "]\nwidget_order = [\"clock\"]\n[" + section
        + ".widget.clock]\ntype = \"clock\"\nflip_x = true\nflip_y = true\n");
    ConfigService service;
    auto applyReset = [&] {
      auto widgets = snapshot(service)[section]["widget"];
      widgets["clock"]["flip_x"] = false;
      widgets["clock"]["flip_y"] = false;
      auto request = guard(service);
      request["entries"] = json::array({{{"path", {section, "widget"}}, {"value", widgets}}});
      request["expected"] = json::array({{{"path", {section, "widget"}},
          {"value", snapshot(service)[section]["widget"]}}});
      const auto result = fixture.request(service, "apply", request);
      check(result.at("ok").get<bool>(), result.dump());
      check(snapshot(service)[section]["widget"]["clock"]["flip_x"] == false
          && snapshot(service)[section]["widget"]["clock"]["flip_y"] == false,
          "Export lost explicit false widget transforms");
    };
    applyReset();
    service.cancelProfilePreview();
    check(snapshot(service)[section]["widget"]["clock"]["flip_x"] == true
        && snapshot(service)[section]["widget"]["clock"]["flip_y"] == true,
        "Cancel did not restore both inherited widget transforms");
    applyReset();
    check(service.commitProfilePreview(), service.lastMutationError());
    ConfigService reloaded;
    check(snapshot(reloaded)[section]["widget"]["clock"]["flip_x"] == false
        && snapshot(reloaded)[section]["widget"]["clock"]["flip_y"] == false,
        "Saved false transforms reverted to inherited true values");
  }
}

void previewCancelAndSave() {
  Fixture fixture;
  ConfigService service;
  int reloads = 0;
  service.addReloadCallback([&] { ++reloads; }, "profile-test");
  radius(service, 24.0);
  check(reloads == 1, "Preview must notify open clients immediately and once");
  check(service.profilePreviewActive() && service.profilePreviewDirty(), "Changed appearance must start dirty preview");
  check(diskRadius(fixture) == 12.0, "Unsaved preview leaked to disk");
  check(snapshot(service, "committed")["shell"]["design"]["radius_xl"] == 12.0, "Committed snapshot includes draft");
  service.cancelProfilePreview();
  check(!service.profilePreviewActive() && !service.profilePreviewDirty(), "Cancel leaves preview active");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 12.0, "Cancel did not restore committed geometry");
  radius(service, 28.0);
  check(service.commitProfilePreview(), service.lastMutationError());
  check(diskRadius(fixture) == 28.0 && !service.profilePreviewActive(), "Save did not persist and close preview");
  ConfigService reopened;
  check(snapshot(reopened)["shell"]["design"]["radius_xl"] == 28.0, "Saved appearance did not survive reopen");
}

void controlCenterPresentationOwnership() {
  Fixture fixture;
  ConfigService service;
  const auto baseline = snapshot(service)["control_center"];
  const std::vector<std::string> fields{"shortcuts", "sidebar", "sidebar_section", "width",
      "hidden_tabs", "show_shortcut_labels", "show_session_button"};
  std::vector<std::vector<std::string>> paths;
  for (const auto& field : fields) {
    paths.push_back({"control_center", field});
    check(noctalia::profile::owns(paths.back()), "Control-center presentation field is not profile-owned");
  }
  check(!noctalia::profile::owns({"control_center"})
      && !noctalia::profile::owns({"control_center", "calendar", "show_week_numbers"})
      && !noctalia::profile::owns({"control_center", "account"})
      && !noctalia::profile::owns({"control_center", "device"}), "Control-center ownership captured independent data");
  const auto edits = std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
      {{"control_center", "shortcuts"}, std::vector<ShortcutConfig>{}},
      {{"control_center", "sidebar"}, std::string("none")},
      {{"control_center", "sidebar_section"}, std::string("full")},
      {{"control_center", "width"}, std::int64_t{900}},
      {{"control_center", "hidden_tabs"}, std::vector<std::string>{"media"}},
      {{"control_center", "show_shortcut_labels"}, false},
      {{"control_center", "show_session_button"}, false},
  };
  check(service.setOverrides(edits), service.lastMutationError());
  check(service.profilePreviewDirty(), "Control-center edit did not start a dirty preview");
  const auto draft = snapshot(service)["control_center"];
  for (const auto& field : fields)
    check(snapshot(service, "committed")["control_center"][field] == baseline[field],
        "Control-center preview persisted before Save");
  check(draft["shortcuts"].empty() && draft["show_shortcut_labels"] == false
      && draft["show_session_button"] == false && draft["width"] == 900,
      "Control-center preview discarded explicit empty/false values");
  // This independent calendar option is written while presentation is a draft.
  check(service.setOverride({"control_center", "calendar", "show_week_numbers"}, true), service.lastMutationError());
  service.cancelProfilePreview();
  for (const auto& field : fields)
    check(snapshot(service)["control_center"][field] == baseline[field], "Cancel did not restore control-center presentation");
  check(service.config().controlCenter.calendarTab.showWeekNumbers, "Cancel reverted independent calendar data");
  check(service.setOverrides(edits), service.lastMutationError());
  check(service.commitProfilePreview(), service.lastMutationError());
  {
    ConfigService reopened;
    for (const auto& field : fields)
      check(snapshot(reopened)["control_center"][field] == draft[field], "Saved control-center presentation did not survive reopen");
  }
  bool changed = false;
  check(service.clearOverrides(paths, &changed) && changed, "Cannot preview control-center Reset");
  for (const auto& field : fields)
    check(snapshot(service)["control_center"][field] == baseline[field], "Reset failed to reveal the control-center default");
  service.cancelProfilePreview();
  for (const auto& field : fields)
    check(snapshot(service)["control_center"][field] == draft[field], "Cancel Reset lost saved presentation");
  check(service.clearOverrides(paths, &changed) && service.commitProfilePreview(), service.lastMutationError());
  ConfigService reopened;
  for (const auto& field : fields)
    check(snapshot(reopened)["control_center"][field] == baseline[field], "Saved Reset did not persist inherited presentation");
  check(reopened.config().controlCenter.calendarTab.showWeekNumbers, "Saved Reset discarded independent calendar data");
}

void explicitApplicationPreparationRetry() {
  Fixture fixture;
  ConfigService service;
  ConfigService::ProfilePreparation preparation{.error = "Repair application settings permissions"};
  service.setProfilePrepareCallback([&] { return preparation; });
  check(!service.canRetryProfilePreparation() && !service.retryProfilePreparation(), "Unbound preparation exposed a retry");
  int retries = 0;
  service.setProfilePrepareRetryCallback([&] {
    if (preparation.pending || preparation.error.empty()) return false;
    ++retries;
    preparation = {.pending = true};
    return true;
  });
  radius(service, 27.0);
  SettingsWindow window;
  SettingsWindowMutationTestAccess::attach(window, service);
  auto banner = SettingsWindowMutationTestAccess::statusRow(window);
  const auto findButton = [&](const auto& self, Node& node) -> Button* {
    if (auto* button = dynamic_cast<Button*>(&node)) return button;
    for (const auto& child : node.children()) if (auto* found = self(self, *child)) return found;
    return nullptr;
  };
  check(banner && service.canRetryProfilePreparation(), "Preparation error has no retry action");
  auto* retry = findButton(findButton, *banner);
  check(retry != nullptr, "Status banner did not expose Retry application appearance");
  retry->inputArea()->setFrameSize(20, 20);
  retry->inputArea()->dispatchPress(10, 10, BTN_LEFT, true);
  retry->inputArea()->dispatchPress(10, 10, BTN_LEFT, false);
  check(retries == 1 && preparation.pending && !service.canRetryProfilePreparation(), "Native retry button did not queue preparation");
  check(!service.retryProfilePreparation() && retries == 1, "Repeated retry queued duplicate work");
  banner = SettingsWindowMutationTestAccess::statusRow(window);
  check(banner && !findButton(findButton, *banner), "Pending preparation still exposes a retry button");
  check(service.profilePreviewDirty() && diskRadius(fixture) == 12.0, "Retry committed or cancelled the native draft");
  preparation = {};
  window.onProfilePreparationChanged();
  check(!service.canRetryProfilePreparation() && !service.retryProfilePreparation(), "Successful preparation permits another retry");
  check(service.profilePreviewDirty() && diskRadius(fixture) == 12.0, "Retry completion implicitly saved the profile");
  check(service.commitProfilePreview() && diskRadius(fixture) == 27.0, "Explicit Save failed after preparation recovery");
  service.setProfilePrepareRetryCallback({});
  service.setProfilePrepareCallback({});
}

void wallpaperFavoritesAreAcknowledgedAndIndependent() {
  Fixture fixture;
  ConfigService service;
  const auto first = (fixture.settings.parent_path() / "first.png").string();
  const auto renamed = (fixture.settings.parent_path() / "renamed.png").string();
  WallpaperFavorite favorite;
  favorite.themeMode = ThemeMode::Light;
  favorite.paletteSource = PaletteSource::Builtin;
  favorite.builtinPalette = "Saved palette";
  radius(service, 23.0);
  check(service.addWallpaperFavorite(first, favorite), service.lastMutationError());
  check(service.addWallpaperFavorite(first), service.lastMutationError());
  check(service.wallpaperFavorite(first)->builtinPalette == "Saved palette", "Starring reset favorite theme");
  check(diskRadius(fixture) == 12.0 && service.profilePreviewDirty(), "Favorite persisted an unsaved appearance draft");
  fixture.failWrites();
  check(!service.removeWallpaperFavorite(first), "Failed favorite removal reported success");
  check(service.isWallpaperFavorite(first), "Failed removal changed live favorites");
  check(!service.moveWallpaperFavorite(first, renamed), "Failed favorite rename reported success");
  check(service.isWallpaperFavorite(first) && !service.isWallpaperFavorite(renamed), "Failed favorite rename changed memory");
  fixture.allowWrites();
  check(service.moveWallpaperFavorite(first, renamed), service.lastMutationError());
  check(!service.isWallpaperFavorite(first) && service.wallpaperFavorite(renamed)->themeMode == ThemeMode::Light,
      "Favorite did not follow rename with its theme");
  service.cancelProfilePreview();
  ConfigService reopened;
  check(reopened.isWallpaperFavorite(renamed), "Appearance Cancel erased independent favorite");
  check(reopened.addWallpaperFavorite(first), reopened.lastMutationError());
  check(!reopened.moveWallpaperFavorite(renamed, first), "Favorite move overwrote destination preference");
}

void applicationPreparationGatesSave() {
  Fixture fixture;
  ConfigService service;
  ConfigService::ProfilePreparation preparation{.pending = true};
  service.setProfilePrepareCallback([&] { return preparation; });
  radius(service, 26.0);
  check(status(service)["preparation"]["pending"] == true, "Pending application preview was not published");
  check(!service.commitProfilePreview(), "Save bypassed a pending application write");
  check(service.profilePreviewActive() && diskRadius(fixture) == 12.0, "Pending Save changed committed state");
  preparation = {.pending = false, .error = "Application caret conflict"};
  check(!service.commitProfilePreview(), "Save bypassed a failed application write");
  check(service.lastMutationError() == preparation.error, "Application write error was hidden");
  service.cancelProfilePreview();
  check(!service.profilePreviewActive() && diskRadius(fixture) == 12.0, "Application error blocked native Cancel");
  radius(service, 29.0);
  preparation = {.pending = false, .skipped = {"Existing custom caret setting"}};
  check(status(service)["preparation"]["skipped"].size() == 1, "Custom setting preservation was not published");
  check(service.commitProfilePreview(), service.lastMutationError());
  check(diskRadius(fixture) == 29.0, "Acknowledged appearance failed to commit");
}

void nativeProfileTransitionWaitsForApplications() {
  const auto drain = [] {
    for (int iteration = 0; iteration < 16; ++iteration) {
      auto callbacks = DeferredCall::takePending();
      if (callbacks.empty()) return;
      for (auto& callback : callbacks) callback();
    }
    check(DeferredCall::takePending().empty(), "Profile callbacks failed to settle");
  };
  Fixture fixture;
  ConfigService service;
  SettingsWindow window;
  SettingsWindowMutationTestAccess::attach(window, service);
  ConfigService::ProfilePreparation preparation{.pending = true};
  service.setProfilePrepareCallback([&] { return preparation; });
  radius(service, 26.0);
  SettingsWindowMutationTestAccess::transition(window, "save");
  drain();
  drain();
  check(SettingsWindowMutationTestAccess::transitionBusy(window), "Native Save did not await adapter");
  check(diskRadius(fixture) == 12.0, "Native Save wrote before adapter acknowledgment");
  preparation = {};
  window.onProfilePreparationChanged();
  drain();
  check(!SettingsWindowMutationTestAccess::transitionBusy(window) && diskRadius(fixture) == 26.0,
      "Native Save did not finish after adapter acknowledgment");
  radius(service, 30.0);
  preparation.pending = true;
  SettingsWindowMutationTestAccess::transition(window, "discard");
  drain();
  drain();
  check(!service.profilePreviewActive() && SettingsWindowMutationTestAccess::transitionBusy(window),
      "Cancel did not restore baseline while awaiting applications");
  preparation = {.error = "Cannot restore editor appearance"};
  window.onProfilePreparationChanged();
  drain();
  check(!SettingsWindowMutationTestAccess::transitionBusy(window)
      && SettingsWindowMutationTestAccess::errorStatus(window), "Cancel hid application restore failure");
  preparation = {.pending = true};
  radius(service, 31.0);
  SettingsWindowMutationTestAccess::transition(window, "save");
  drain();
  drain();
  SettingsWindowMutationTestAccess::closeWindow(window);
  preparation = {};
  window.onProfilePreparationChanged();
  drain();
  check(diskRadius(fixture) == 26.0, "Closed settings completed a stale Save");
}

void detachedPopupMaterialScope() {
  Node settings;
  settings.setMaterialSurface("settings");
  auto* opener = settings.addChild(std::make_unique<Node>());
  XdgPopupParent parent{.materialSurface = std::string(opener->materialSurfaceName())};
  Node popup;
  auto* control = popup.addChild(std::make_unique<Node>());
  popup.setMaterialSurface(parent.materialSurface);
  check(control->materialSurfaceName() == "settings", "Detached popup omitted opener surface scope");
  Style::MaterialOverrides overrides;
  overrides.surfaces["settings"].elevation = 0.0F;
  overrides.surfaces["settings"].primitive = noctalia::material::Primitive::Flat;
  overrides.surfaces["bar"].elevation = 8.0F;
  noctalia::material::Parameters global;
  global.plateau.elevation = 4.0F;
  check(Style::resolveMaterial(global, overrides, "control", "select", control->materialSurfaceName()).plateau.elevation == 0.0F,
      "Popup child lost explicit zero surface override");
  popup.setMaterialSurface("bar");
  check(Style::resolveMaterial(global, overrides, "control", "select", control->materialSurfaceName()).plateau.elevation == 8.0F,
      "Reused popup retained previous opener scope");
  control->setMaterialSurface("settings");
  popup.setMaterialSurface("");
  check(control->materialSurfaceName() == "settings", "Explicit nested scope was erased by parent reset");
  control->setMaterialSurface("");
  check(control->materialSurfaceName().empty(), "Empty parent scope did not reset inheritance");
}

void controlPresentationPreviewPersistence() {
  Fixture fixture;
  ConfigService service;
  const auto before = snapshot(service)["shell"]["controls"];
  check(service.setOverride({"shell", "controls", "toggle_variant"}, std::string("sweep")), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "checkbox_variant"}, std::string("plateau")), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "toggle_transition_ms"}, 0.0), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "toggle_mirror_rtl"}, false), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "checkbox_plateau_tick"}, false), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "card_face_role"}, std::string("secondary")), service.lastMutationError());
  check(service.profilePreviewActive() && service.profilePreviewDirty(), "Control presentation did not start preview");
  check(snapshot(service, "committed")["shell"]["controls"] == before, "Unsaved control variants leaked to committed config");
  check(!service.setOverride({"shell", "controls", "toggle_variant"}, std::string("unsupported")), "Unknown control variant accepted");
  check(snapshot(service)["shell"]["controls"]["toggle_variant"] == "sweep", "Rejected variant damaged draft");
  const auto validDraft = snapshot(service);
  check(!service.setOverrides({
      {{"shell", "controls", "toggle_variant"}, std::string("unsupported")},
      {{"shell", "design", "radius_xl"}, 35.0},
      {{"shell", "animation", "style"}, std::string("linear")}}), "Mixed invalid edit batch was accepted");
  check(snapshot(service) == validDraft, "Invalid batch partially applied valid edits or erased existing values");
  check(!service.lastMutationError().empty(), "Rejected batch omitted its validation error");
  check(!service.setOverride({"shell", "animation", "style"}, std::string("bounce")), "Unknown motion override accepted");
  check(snapshot(service) == validDraft, "Invalid motion override changed the draft");
  service.cancelProfilePreview();
  check(snapshot(service)["shell"]["controls"] == before, "Cancel did not restore control variants");
  check(service.setOverride({"shell", "controls", "toggle_variant"}, std::string("sweep")), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "toggle_transition_ms"}, 0.0), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "toggle_mirror_rtl"}, false), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "segmented_variant"}, std::string("floating")), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "segmented_indicator_inset"}, -4.0), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "segmented_travel_stretch"}, 0.0), service.lastMutationError());
  check(service.setOverride({"shell", "controls", "segmented_indicator_opacity"}, 0.0), service.lastMutationError());
  check(service.commitProfilePreview(), service.lastMutationError());
  ConfigService reopened;
  const auto stored = snapshot(reopened)["shell"]["controls"];
  check(stored["toggle_variant"] == "sweep" && stored["toggle_transition_ms"] == 0.0
      && stored["toggle_mirror_rtl"] == false, "Saved variant/explicit zero/false did not survive reopen");
  check(stored["segmented_variant"] == "floating" && stored["segmented_indicator_inset"] == -4.0
      && stored["segmented_travel_stretch"] == 0.0 && stored["segmented_indicator_opacity"] == 0.0,
      "Segmented variant/negative inset/explicit zero did not survive reopen");
}

void pendingExternalPaletteAndCancel() {
  Fixture fixture;
  ConfigService service;
  radius(service, 24.0);
  fixture.disk(12.0, "External palette", false);
  // Deliberately do not call checkReload: the external write is still pending.
  check(service.setOverride({"theme", "pure_black_dark"}, true), service.lastMutationError());
  assertPalette(service, "External palette", true);
  service.cancelProfilePreview();
  assertPalette(service, "External palette", true);
  check(diskRadius(fixture) == 12.0, "Cancel persisted preview geometry with unrelated setting");
  ConfigService reopened;
  assertPalette(reopened, "External palette", true);
}

void writeFailureRebaseAndRetry() {
  Fixture fixture;
  ConfigService service;
  radius(service, 24.0);
  fixture.disk(12.0, "External palette", false);
  fixture.failWrites();
  check(!service.setOverride({"theme", "pure_black_dark"}, true), "Blocked atomic write unexpectedly succeeded");
  check(!service.lastMutationError().empty(), "Failed write has no error");
  check(toml::parse_file(fixture.settings.string())["theme"]["builtin"].value<std::string>() == "External palette",
      "Failed write damaged external palette");
  fixture.allowWrites();
  // This retry must rebase against disk again after the failed caller rolls back.
  check(service.setOverride({"theme", "pure_black_dark"}, true), service.lastMutationError());
  assertPalette(service, "External palette", true);
  check(service.profilePreviewDirty(), "Failed unrelated save discarded preview");
  check(service.commitProfilePreview(), service.lastMutationError());
  ConfigService reopened;
  assertPalette(reopened, "External palette", true);
  check(diskRadius(fixture) == 24.0, "Retry failed to preserve draft for explicit Save");
}

void saveFailureRetainsCancelableDraft() {
  Fixture fixture;
  ConfigService service;
  radius(service, 24.0);
  fixture.failWrites();
  check(!service.commitProfilePreview(), "Blocked preview Save unexpectedly succeeded");
  check(service.profilePreviewActive() && service.profilePreviewDirty(), "Save failure discarded draft");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 24.0, "Save failure changed visible draft");
  check(diskRadius(fixture) == 12.0, "Save failure changed committed appearance");
  service.cancelProfilePreview();
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 12.0, "Cancel after Save failure did not restore baseline");
  fixture.allowWrites();
  radius(service, 30.0);
  check(service.commitProfilePreview(), service.lastMutationError());
  check(diskRadius(fixture) == 30.0, "A failed Save prevented a later valid Save");
}

void externalAppearanceConflict() {
  Fixture fixture;
  ConfigService service;
  radius(service, 24.0);
  fixture.disk(18.0, "External palette", false);
  check(!service.commitProfilePreview(), "Save overwrote a conflicting external appearance");
  check(service.profilePreviewConflict() && service.profilePreviewDirty(), "Conflict did not retain dirty preview");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 24.0, "Conflict discarded visible draft");
  check(diskRadius(fixture) == 18.0, "Conflict damaged external appearance");
  service.cancelProfilePreview();
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 18.0, "Cancel did not adopt external committed appearance");
  assertPalette(service, "External palette", false);
}

void guardedIpcAndDirtyReset() {
  Fixture fixture;
  ConfigService service;
  const auto before = guard(service);
  check(fixture.request(service, "begin", before).at("ok").get<bool>(), "Guarded begin failed");
  const auto first = guard(service);
  check(!fixture.request(service, "cancel", before).at("ok").get<bool>(), "Stale generation canceled active preview");
  auto external = first;
  external["dirty"] = true;
  check(fixture.request(service, "external-dirty", external).at("ok").get<bool>(), "External dirty update failed");
  check(service.profilePreviewDirty(), "External changes not reflected in dirty state");
  external["dirty"] = false;
  check(fixture.request(service, "external-dirty", external).at("ok").get<bool>(), "External dirty reset failed");
  check(!service.profilePreviewDirty(), "Returning external settings to baseline remains dirty");
  check(fixture.request(service, "cancel", first).at("ok").get<bool>(), "Current guard could not cancel");
  check(!fixture.request(service, "commit", first).at("ok").get<bool>(), "Ended preview accepted Save");
  check(fixture.request(service, "begin", guard(service)).at("ok").get<bool>(), "Second preview could not begin");
  for (const auto action : {"begin", "apply", "commit", "cancel", "external-dirty", "resources"}) {
    check(!fixture.request(service, action, first).at("ok").get<bool>(), "Old generation affected replacement preview");
  }
  auto wrongSession = guard(service);
  wrongSession["session"] = "previous-process";
  check(!fixture.request(service, "cancel", wrongSession).at("ok").get<bool>(), "Foreign session canceled preview");
  check(service.profilePreviewActive(), "Rejected request altered active preview");
  auto resourceRequest = guard(service);
  resourceRequest["fonts"] = json::array();
  const auto originalConfig = snapshot(service);
  const auto originalGuard = guard(service);
  const char* priorDataHome = std::getenv("XDG_DATA_HOME");
  const std::optional<std::string> savedDataHome = priorDataHome ? std::optional<std::string>(priorDataHome) : std::nullopt;
  check(::setenv("XDG_DATA_HOME", "relative-must-not-be-read", 1) == 0, "Cannot isolate empty resource request");
  const auto emptyResources = fixture.request(service, "resources", resourceRequest);
  if (savedDataHome) ::setenv("XDG_DATA_HOME", savedDataHome->c_str(), 1);
  else ::unsetenv("XDG_DATA_HOME");
  check(emptyResources.at("ok").get<bool>(),
      "Guarded empty resource registration failed");
  const auto effectDataHome = fixture.root / "effect-data";
  check(::setenv("XDG_DATA_HOME", effectDataHome.c_str(), 1) == 0,
      "Cannot isolate custom effect resources");
  const std::string effectSource =
      "vec4 noctalia_effect(vec4 s, vec4 b, vec2 uv, vec2 px, vec2 size, vec4 p0, vec4 p1, "
      "vec4 p2, vec4 p3, vec4 p4, vec4 p5, vec4 p6, vec4 p7) { return s; }";
  resourceRequest["effects"] = json::array({{
      {"stable_id", "user.profile-effect"},
      {"sha256", "61643eb38679e241758d63df3256e6e757b9705695acfbe4bd5d15c44bbc0f88"},
      {"size", effectSource.size()}, {"abi", 1}, {"max_sample_radius", 8.0},
      {"source", effectSource},
  }});
  const auto effectResources = fixture.request(service, "resources", resourceRequest);
  check(effectResources.at("ok").get<bool>()
          && effectResources.at("resources").at("effects").size() == 1,
      "Guarded custom effect resource registration failed");
  auto tamperedEffectRequest = resourceRequest;
  tamperedEffectRequest["effects"][0]["source"] = effectSource + " ";
  tamperedEffectRequest["effects"][0]["size"] = effectSource.size() + 1;
  check(!fixture.request(service, "resources", tamperedEffectRequest).at("ok").get<bool>(),
      "Custom effect resource accepted bytes that differ from its digest");
  if (savedDataHome) ::setenv("XDG_DATA_HOME", savedDataHome->c_str(), 1);
  else ::unsetenv("XDG_DATA_HOME");
  check(guard(service) == originalGuard && snapshot(service) == originalConfig && service.profilePreviewActive(),
      "Resource registration mutated appearance or preview ownership");
}

void customEffectResourceExport() {
  Fixture fixture;
  ConfigService service;
  const auto dataHome = fixture.root / "effect-export";
  check(::setenv("XDG_DATA_HOME", dataHome.c_str(), 1) == 0, "Cannot isolate effect export");
  const std::string source =
      "vec4 noctalia_effect(vec4 s, vec4 b, vec2 uv, vec2 px, vec2 size, vec4 p0, vec4 p1, "
      "vec4 p2, vec4 p3, vec4 p4, vec4 p5, vec4 p6, vec4 p7) { return s; }";
  const auto installed = custom_effect_assets::installSealed(
      source, "user.profile-effect",
      "61643eb38679e241758d63df3256e6e757b9705695acfbe4bd5d15c44bbc0f88", 8.0F);
  check(static_cast<bool>(installed), installed.error);
  check(service.setOverrides({
      {{"shell", "material_overrides", "surfaces", "bar.widget.profile", "custom_effect"},
       std::string("user.profile-effect")},
      {{"shell", "material_overrides", "surfaces", "bar.widget.profile", "custom_effect_digest"},
       std::string("61643eb38679e241758d63df3256e6e757b9705695acfbe4bd5d15c44bbc0f88")},
      {{"shell", "material_overrides", "surfaces", "bar.widget.profile", "custom_sample_radius"}, 8.0},
      {{"shell", "material_overrides", "surfaces", "bar.widget.profile-second", "custom_effect"},
       std::string("user.profile-effect")},
      {{"shell", "material_overrides", "surfaces", "bar.widget.profile-second", "custom_effect_digest"},
       std::string("61643eb38679e241758d63df3256e6e757b9705695acfbe4bd5d15c44bbc0f88")},
      {{"shell", "material_overrides", "surfaces", "bar.widget.profile-second", "custom_sample_radius"}, 12.0},
  }), service.lastMutationError());
  const auto exported = json::parse(service.profileRequest("resource-export"));
  check(exported.at("ok").get<bool>() && exported.at("resources").at("effects").size() == 1,
      "Custom effect resource export omitted the active asset");
  check(exported.at("resources").at("effects").at(0).at("max_sample_radius") == 12.0,
      "Shared custom effect resource did not preserve the largest sibling sampling bound");
  const auto serialized = exported.at("resources").dump();
  check(!serialized.contains(fixture.root.string()) && serialized.contains(source),
      "Custom effect resource export leaked a path or omitted sealed bytes");
}

void guardedApplyAndAbsentReset() {
  Fixture fixture;
  ConfigService service;
  auto payload = guard(service);
  payload["entries"] = json::array({{{"path", {"shell", "design", "radius_xl"}}, {"value", 22.0}}});
  payload["expected"] = json::array({{{"path", {"shell", "design", "radius_xl"}}, {"value", 12.0}}});
  check(fixture.request(service, "apply", payload).at("ok").get<bool>(), "Guarded value apply failed");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 22.0, "Guarded apply not visible");
  payload.update(guard(service));
  payload["entries"][0]["value"] = 26.0;
  check(!fixture.request(service, "apply", payload).at("ok").get<bool>(), "Stale expected value overwrote draft");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 22.0, "Rejected expected value changed draft");
  auto reset = guard(service);
  reset["entries"] = json::array({{{"path", {"bar", "absent-test-bar", "radius"}}, {"value", nullptr}}});
  reset["expected"] = reset["entries"];
  const auto bars = snapshot(service)["bar"];
  check(fixture.request(service, "apply", reset).at("ok").get<bool>(), "Reset of absent property failed");
  check(snapshot(service)["bar"] == bars, "Reset of absent property created a default bar");
}

void backgroundReconcileDoesNotReopenPreview() {
  Fixture fixture;
  ConfigService service;
  const auto payload = [&] {
    auto request = guard(service);
    request["entries"] = json::array({{{"path", {"shell", "design", "radius_xl"}}, {"value", 24.0}}});
    request["expected"] = json::array({{{"path", {"shell", "design", "radius_xl"}}, {"value", 12.0}}});
    return request;
  };
  const auto inactive = payload();
  check(fixture.request(service, "reconcile", inactive).at("ok").get<bool>(), "Inactive reconcile failed");
  check(status(service).at("active") == false && !service.profilePreviewDirty(), "Background reconcile opened a preview");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 12.0 && diskRadius(fixture) == 12.0, "Inactive reconcile changed appearance");
  service.beginProfilePreview();
  const auto active = payload();
  check(fixture.request(service, "reconcile", active).at("ok").get<bool>(), "Active reconcile failed");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 24.0 && service.profilePreviewDirty(), "Active reconcile did not update preview");
  service.cancelProfilePreview();
  check(fixture.request(service, "reconcile", active).at("ok").get<bool>(), "Late reconcile failed after Cancel");
  check(status(service).at("active") == false && !service.profilePreviewDirty(), "Late reconcile reopened Cancelled preview");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 12.0 && diskRadius(fixture) == 12.0, "Late reconcile changed committed appearance");
  service.beginProfilePreview();
  check(!fixture.request(service, "reconcile", active).at("ok").get<bool>(), "Stale reconcile entered a replacement preview");
  check(snapshot(service)["shell"]["design"]["radius_xl"] == 12.0 && !service.profilePreviewDirty(), "Stale reconcile changed replacement preview");
  service.cancelProfilePreview();
}

void processRestartDiscardsDraft() {
  Fixture fixture;
  ConfigService service;
  radius(service, 24.0);
  check(service.setOverride({"theme", "pure_black_dark"}, true), service.lastMutationError());
  const auto priorSession = status(service).at("session");
  const auto output = fixture.root / "restarted.json";
  const auto staleRequest = fixture.root / "stale.json";
  writeFile(staleRequest, guard(service).dump());
  check(::chmod(staleRequest.c_str(), 0600) == 0, "Cannot protect restart request");
  const pid_t child = ::fork();
  check(child >= 0, "Cannot create restart probe process");
  if (child == 0) {
    ::execl("/proc/self/exe", "profile_preview_test", "--restart-probe", output.c_str(), staleRequest.c_str(),
        static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int result = 0;
  pid_t waited;
  do { waited = ::waitpid(child, &result, 0); } while (waited < 0 && errno == EINTR);
  check(waited == child && WIFEXITED(result) && WEXITSTATUS(result) == 0, "Restart probe failed");
  const auto restarted = readJson(output);
  check(restarted["status"]["session"] != priorSession, "Restart did not change process session");
  check(restarted["status"]["active"] == false, "Restart resurrected an unsaved preview");
  check(restarted["config"]["shell"]["design"]["radius_xl"] == 12.0, "Restart loaded unsaved appearance");
  check(restarted["config"]["theme"]["pure_black_dark"] == true, "Restart lost separately saved setting");
  check(restarted["stale_cancel"]["ok"] == false, "Restart accepted previous process guard");
}
void durableCommitReceipt() {
  Fixture fixture;
  ConfigService service;
  const auto original = status(service).at("last_commit");
  service.beginProfilePreview();
  const auto draft = status(service);
  const json expected{{"session", draft.at("session")}, {"generation", draft.at("generation")}};
  fixture.failWrites();
  check(!service.commitProfilePreview(), "Blocked WM-only Save unexpectedly succeeded");
  check(status(service).at("last_commit") == original, "Rejected Save published a commit receipt");
  fixture.allowWrites();
  check(service.commitProfilePreview(), service.lastMutationError());
  check(status(service).at("last_commit") == expected, "WM-only Save has no durable receipt");
  ConfigService restarted;
  check(status(restarted).at("last_commit") == expected, "Commit receipt did not survive ConfigService restart");
  service.beginProfilePreview();
  service.cancelProfilePreview();
  check(status(service).at("last_commit") == expected, "Cancel replaced the successful Save receipt");
}

void animationSpeedZeroRepresentation() {
  Config cfg;
  cfg.shell.animation.enabled = false;
  cfg.shell.animation.speed = 1.75F;

  const auto speedSlider = [](const Config& current) {
    auto entries = settings::buildSettingsRegistry(current, nullptr, nullptr);
    const auto entry = std::ranges::find_if(entries, [](const auto& row) {
      return row.path == std::vector<std::string>{"shell", "animation", "speed"};
    });
    check(entry != entries.end(), "Animation speed slider is missing");
    return std::get<settings::SliderSetting>(entry->control);
  };

  auto slider = speedSlider(cfg);
  check(slider.value == 0.0 && slider.minValue == 0.0,
      "Disabled animation config did not reload as visible speed 0");
  check(slider.linkedPaths == std::vector<std::vector<std::string>>{{"shell", "animation", "enabled"}},
      "Animation speed reset does not include the enabled representation");

  auto writes = slider.groupedCommit(0.0);
  check(writes == std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
      {{"shell", "animation", "enabled"}, false}},
      "Speed 0 did not map exclusively to the existing disabled representation");
  check(cfg.shell.animation.speed == 1.75F, "Displaying speed 0 destroyed the retained positive speed");

  writes = slider.groupedCommit(2.25);
  check(writes == std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>{
      {{"shell", "animation", "speed"}, 2.25},
      {{"shell", "animation", "enabled"}, true}},
      "Positive animation speed did not atomically persist speed and re-enable motion");

  writes = slider.groupedCommit(0.05);
  check(std::get<double>(writes.front().second) == 0.1,
      "The first positive slider step escaped the persisted schema range");

  cfg.shell.animation.enabled = true;
  slider = speedSlider(cfg);
  check(slider.value == 1.75, "Re-enabled animation config did not restore its retained positive speed");
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::string_view(argv[1]) == "--restart-probe") {
    try {
      ConfigService service;
      writeFile(argv[2], json{{"status", status(service)}, {"config", snapshot(service)},
          {"stale_cancel", json::parse(service.profileRequest(std::string("cancel ") + argv[3]))}}.dump());
      return 0;
    } catch (const std::exception& error) {
      std::println(stderr, "profile_preview restart probe: {}", error.what());
      return 1;
    }
  }
  int failed = 0;
  for (const auto& [name, test] : std::vector<std::pair<std::string_view, std::function<void()>>>{
           {"empty desktop widget export/import", emptyDesktopWidgetProfileRoundtrip},
           {"widget flip reset export/import", widgetFlipResetRoundtrip},
           {"preview/cancel/save", previewCancelAndSave},
           {"background reconcile preserves Cancel", backgroundReconcileDoesNotReopenPreview},
           {"application preparation gates Save", applicationPreparationGatesSave},
           {"explicit application preparation retry", explicitApplicationPreparationRetry},
           {"control-center presentation ownership", controlCenterPresentationOwnership},
           {"native profile transition waits for applications", nativeProfileTransitionWaitsForApplications},
           {"wallpaper favorite acknowledged persistence", wallpaperFavoritesAreAcknowledgedAndIndependent},
           {"settings curve and grouped reset", settingsCurveAndGroupedReset},
           {"design token mutation validation", designTokenMutationValidation},
           {"corner-power profile and Input focus", cornerPowerProfileAndInputFocus},
           {"owned animation routing", ownedAnimationRouting},
           {"animation speed zero representation", animationSpeedZeroRepresentation},
           {"deferred settings mutation lifetime", deferredSettingsMutationLifetime},
           {"exact material plane and appearance registry", exactMaterialPlaneAndSnapshotTransport},
           {"lock widget appearance ownership/transport", lockWidgetAppearanceOwnershipAndTransport},
           {"durable commit receipt", durableCommitReceipt},
           {"detached popup scope inheritance", detachedPopupMaterialScope},
           {"control presentation preview/persistence", controlPresentationPreviewPersistence},
           {"pending external palette/cancel", pendingExternalPaletteAndCancel},
           {"failed write/rebase/retry", writeFailureRebaseAndRetry},
           {"Save failure/cancel/retry", saveFailureRetainsCancelableDraft},
           {"external appearance conflict", externalAppearanceConflict},
           {"IPC generations/dirty reset", guardedIpcAndDirtyReset},
           {"custom effect resource export", customEffectResourceExport},
           {"IPC expected value/absent reset", guardedApplyAndAbsentReset},
           {"process restart", processRestartDiscardsDraft}}) {
    try {
      test();
      std::println("profile_preview: PASS {}", name);
    } catch (const std::exception& error) {
      ++failed;
      std::println(stderr, "profile_preview: FAIL {}: {}", name, error.what());
    }
  }
  return failed ? 1 : 0;
}
