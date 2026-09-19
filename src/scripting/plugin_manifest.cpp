#include "scripting/plugin_manifest.h"

#include "core/input/key_chord.h"
#include "config/config_export.h"
#include "core/log.h"
#include "core/toml.h" // IWYU pragma: keep
#include "scripting/plugin_api.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_panel_shell.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace scripting {

  namespace {

    constexpr Logger kLog("plugin-manifest");

    std::optional<double> numericSetting(const WidgetSettingValue& value) {
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*i);
      }
      if (const auto* d = std::get_if<double>(&value)) {
        return *d;
      }
      return std::nullopt;
    }

    bool valueEqual(const WidgetSettingValue& a, const WidgetSettingValue& b) {
      const auto aNum = numericSetting(a);
      const auto bNum = numericSetting(b);
      if (aNum.has_value() || bNum.has_value()) {
        return aNum.has_value() && bNum.has_value() && *aNum == *bNum;
      }
      if (a.index() != b.index()) {
        return false;
      }
      return std::visit(
          [&](const auto& av) {
            using T = std::decay_t<decltype(av)>;
            const auto* bv = std::get_if<T>(&b);
            return bv != nullptr && av == *bv;
          },
          a
      );
    }

    // Each entry kind paired with its TOML array-table name.
    constexpr std::array<std::pair<PluginEntryKind, std::string_view>, 6> kEntryKinds{{
        {PluginEntryKind::Widget, "widget"},
        {PluginEntryKind::Panel, "panel"},
        {PluginEntryKind::Shortcut, "shortcut"},
        {PluginEntryKind::DesktopWidget, "desktop_widget"},
        {PluginEntryKind::LauncherProvider, "launcher_provider"},
        {PluginEntryKind::Service, "service"},
    }};

    ManifestFieldType parseFieldType(std::string_view type) {
      if (type == "bool" || type == "boolean") {
        return ManifestFieldType::Bool;
      }
      if (type == "int" || type == "integer") {
        return ManifestFieldType::Int;
      }
      if (type == "double" || type == "number" || type == "float") {
        return ManifestFieldType::Double;
      }
      if (type == "string_list") {
        return ManifestFieldType::StringList;
      }
      if (type == "string_map") {
        return ManifestFieldType::StringMap;
      }
      if (type == "select" || type == "enum") {
        return ManifestFieldType::Select;
      }
      if (type == "file") {
        return ManifestFieldType::File;
      }
      if (type == "folder") {
        return ManifestFieldType::Folder;
      }
      if (type == "glyph") {
        return ManifestFieldType::Glyph;
      }
      if (type == "color") {
        return ManifestFieldType::Color;
      }
      return ManifestFieldType::String;
    }

    std::string tableString(const toml::table& tbl, std::string_view key, std::string fallback = {}) {
      if (auto value = tbl[key].value<std::string>()) {
        return *value;
      }
      return fallback;
    }

    bool tableBool(const toml::table& tbl, std::string_view key, bool fallback) {
      return tbl[key].value<bool>().value_or(fallback);
    }

    std::vector<std::string> tableStringArray(const toml::table& tbl, std::string_view key) {
      std::vector<std::string> out;
      const auto* values = tbl[key].as_array();
      if (values == nullptr) {
        return out;
      }
      for (const auto& node : *values) {
        if (auto value = node.value<std::string>()) {
          out.push_back(*value);
        }
      }
      return out;
    }

    // A TOML number written as either an integer or a float.
    std::optional<double> tableNumber(const toml::table& tbl, std::string_view key) {
      const auto node = tbl[key];
      if (auto i = node.value<std::int64_t>()) {
        return static_cast<double>(*i);
      }
      if (auto d = node.value<double>()) {
        return d;
      }
      return std::nullopt;
    }

    bool parseFieldDefault(const toml::table& field, ManifestField& out, std::string& error) {
      const auto node = field["default"];
      switch (out.type) {
      case ManifestFieldType::Bool:
        out.boolDefault = node.value<bool>().value_or(false);
        break;
      case ManifestFieldType::Int: {
        const auto value = node.value<std::int64_t>();
        if (!value.has_value()) {
          error = "setting '" + out.key + "' int default must be an integer";
          return false;
        }
        out.numberDefault = static_cast<double>(*value);
        break;
      }
      case ManifestFieldType::Double: {
        const auto value =
            node.value<std::int64_t>().transform(
                                          [](std::int64_t integer) { return static_cast<double>(integer); }
            ).or_else([&node]() { return node.value<double>(); });
        if (!value.has_value() || !std::isfinite(*value)) {
          error = "setting '" + out.key + "' double default must be a finite number";
          return false;
        }
        out.numberDefault = *value;
        break;
      }
      case ManifestFieldType::StringList:
        if (const auto* values = node.as_array()) {
          for (const auto& valueNode : *values) {
            if (auto value = valueNode.value<std::string>()) {
              out.stringListDefault.push_back(*value);
            }
          }
        }
        break;
      case ManifestFieldType::StringMap:
        if (const auto* values = node.as_table()) {
          for (const auto& [key, valueNode] : *values) {
            const auto value = valueNode.value<std::string>();
            if (!value.has_value()) {
              error = "setting '" + out.key + "' string_map default values must be strings";
              return false;
            }
            out.stringMapDefault.emplace(std::string(key.str()), *value);
          }
        } else if (node) {
          error = "setting '" + out.key + "' string_map default must be a table";
          return false;
        }
        break;
      default:
        out.stringDefault = node.value<std::string>().value_or(std::string{});
        break;
      }
      return true;
    }

    bool parseFieldNumberOptions(const toml::table& field, ManifestField& out, std::string& error) {
      const bool isInteger = out.type == ManifestFieldType::Int;
      const bool isDouble = out.type == ManifestFieldType::Double;
      const bool isNumeric = isInteger || isDouble;

      const auto parseOption = [&](std::string_view key, std::optional<double>& destination) {
        if (!field.contains(key)) {
          return true;
        }
        if (!isNumeric) {
          error = "setting '" + out.key + "' " + std::string(key) + " is only valid for int or double";
          return false;
        }

        const auto node = field[key];
        std::optional<double> value;
        if (const auto integer = node.value<std::int64_t>()) {
          value = static_cast<double>(*integer);
        } else if (isDouble) {
          value = node.value<double>();
        }
        if (!value.has_value() || !std::isfinite(*value)) {
          error = "setting '"
              + out.key
              + "' "
              + std::string(key)
              + (isInteger ? " must be an integer" : " must be a finite number");
          return false;
        }
        destination = value;
        return true;
      };

      if (!parseOption("min", out.minValue) || !parseOption("max", out.maxValue)) {
        return false;
      }

      std::optional<double> step;
      if (!parseOption("step", step)) {
        return false;
      }
      if (step.has_value()) {
        if (*step <= 0.0) {
          error = "setting '" + out.key + "' step must be greater than zero";
          return false;
        }
        out.step = *step;
      }

      if (!isNumeric) {
        return true;
      }
      if (out.minValue.has_value() && out.maxValue.has_value() && *out.minValue > *out.maxValue) {
        error = "setting '" + out.key + "' min must be less than or equal to max";
        return false;
      }
      if (out.minValue.has_value() && out.numberDefault < *out.minValue) {
        error = "setting '" + out.key + "' default must be greater than or equal to min";
        return false;
      }
      if (out.maxValue.has_value() && out.numberDefault > *out.maxValue) {
        error = "setting '" + out.key + "' default must be less than or equal to max";
        return false;
      }
      return true;
    }

    bool parseFieldOptions(const toml::table& field, ManifestField& out, std::string& error) {
      const auto* options = field["options"].as_array();
      if (options == nullptr) {
        return true;
      }
      for (const auto& node : *options) {
        const auto* optTable = node.as_table();
        if (optTable == nullptr) {
          error = "setting '" + out.key + "' option must be a table with value and label_key";
          return false;
        }
        ManifestSelectOption opt;
        opt.value = tableString(*optTable, "value");
        if (opt.value.empty()) {
          error = "setting '" + out.key + "' option is missing 'value'";
          return false;
        }
        if (optTable->contains("label")) {
          error = "setting '"
              + out.key
              + "' option '"
              + opt.value
              + "' uses 'label'; use 'label_key' that points to translation key instead";
          return false;
        }
        opt.labelKey = tableString(*optTable, "label_key");
        if (opt.labelKey.empty()) {
          error = "setting '" + out.key + "' option '" + opt.value + "' is missing 'label_key'";
          return false;
        }
        out.options.push_back(std::move(opt));
      }
      return true;
    }

    void parseFieldExtensions(const toml::table& field, ManifestField& out) {
      const auto* extensions = field["extensions"].as_array();
      if (extensions == nullptr) {
        return;
      }
      for (const auto& node : *extensions) {
        if (auto value = node.value<std::string>()) {
          out.extensions.push_back(*value);
        }
      }
    }

    void parseFieldVisibility(const toml::table& field, ManifestField& out) {
      const auto* visible = field["visible_when"].as_table();
      if (visible == nullptr) {
        return;
      }
      const auto condition = [](const toml::table& table) -> std::optional<ManifestVisibilityCondition> {
        ManifestVisibilityCondition result;
        result.key = tableString(table, "key");
        const auto* values = table["values"].as_array();
        if (values == nullptr) return std::nullopt;
        for (const auto& node : *values) {
          if (auto value = node.value<std::string>()) {
            result.values.push_back(*value);
          } else if (auto boolean = node.value<bool>()) {
            result.values.emplace_back(*boolean ? "true" : "false");
          }
        }
        if (result.key.empty() || result.values.empty()) return std::nullopt;
        return result;
      };
      ManifestVisibility vis;
      if (auto legacy=condition(*visible)) vis.any.push_back(std::move(*legacy));
      for (const auto* group : {"any", "all"}) {
        const auto* conditions=(*visible)[group].as_array(); if (!conditions) continue;
        auto& target=std::string_view(group)=="any" ? vis.any : vis.all;
        for (const auto& node:*conditions) if (const auto* table=node.as_table())
          if (auto parsed=condition(*table)) target.push_back(std::move(*parsed));
      }
      if (!vis.any.empty() || !vis.all.empty()) out.visibleWhen=std::move(vis);
    }

    bool parseSpringResponseGroup(const toml::table& field, ManifestField& out, std::string& error) {
      if (!field.contains("spring_response")) return true;
      const auto* table=field["spring_response"].as_table();
      const auto fail=[&]{error="setting '"+out.key+"' spring_response requires three distinct numeric keys and label_key";return false;};
      if (!table) return fail(); const auto* keys=(*table)["keys"].as_array(); if (!keys || keys->size()!=3)return fail();
      ManifestSpringResponseGroup group; std::unordered_set<std::string> seen;
      for(std::size_t i=0;i<3;++i){auto key=(*keys)[i].value<std::string>();if(!key||key->empty()||!seen.insert(*key).second)return fail();group.keys[i]=*key;}
      group.labelKey=tableString(*table,"label_key");group.descriptionKey=tableString(*table,"description_key");
      if(group.keys[0]!=out.key||group.labelKey.empty())return fail();out.springResponse=std::move(group);return true;
    }

    bool parseCurveGroup(const toml::table& field, ManifestField& out, std::string& error) {
      if (!field.contains("curve")) return true;
      const auto* table = field["curve"].as_table();
      const auto fail = [&] {
        error = "setting '" + out.key + "' curve requires four distinct keys, label_key, and paired activation strings";
        return false;
      };
      if (!table) return fail();
      const auto* keys = (*table)["keys"].as_array();
      if (!keys || keys->size() != 4) return fail();
      ManifestCurveGroup group;
      std::unordered_set<std::string> seen;
      for (std::size_t i = 0; i < group.keys.size(); ++i) {
        const auto value = (*keys)[i].value<std::string>();
        if (!value || value->empty() || !seen.insert(*value).second) return fail();
        group.keys[i] = *value;
      }
      group.labelKey = tableString(*table, "label_key");
      group.descriptionKey = tableString(*table, "description_key");
      group.activationKey = tableString(*table, "activation_key");
      group.activationValue = tableString(*table, "activation_value");
      if (group.keys[0] != out.key || group.labelKey.empty()) return fail();
      for (const auto* key : {"description_key", "activation_key", "activation_value"}) {
        if (table->contains(key) && !(*table)[key].is_string()) return fail();
      }
      if (table->contains("activation_key") != table->contains("activation_value") ||
          (table->contains("activation_key") && (group.activationKey.empty() || group.activationValue.empty()))) return fail();
      out.curve = std::move(group);
      return true;
    }

    bool validateCurveGroups(const std::vector<ManifestField>& fields, std::string& error) {
      std::unordered_set<std::string> groupedKeys;
      for (const auto& anchor : fields) {
        if (!anchor.curve) continue;
        const auto& group = *anchor.curve;
        const auto fail = [&] {
          error = "setting '" + anchor.key + "' curve requires unshared numeric X[0,1]/Y[-2,2] fields and a valid activation select option";
          return false;
        };
        for (std::size_t i = 0; i < group.keys.size(); ++i) {
          const auto field = std::ranges::find(fields, group.keys[i], &ManifestField::key);
          const double low = i % 2 == 0 ? 0.0 : -2.0;
          const double high = i % 2 == 0 ? 1.0 : 2.0;
          if (field == fields.end() || field->type != ManifestFieldType::Double ||
              field->minValue != low || field->maxValue != high ||
              !groupedKeys.insert(field->key).second) return fail();
        }
        if (!group.activationKey.empty()) {
          const auto field = std::ranges::find(fields, group.activationKey, &ManifestField::key);
          if (field == fields.end() || field->type != ManifestFieldType::Select ||
              std::ranges::none_of(field->options, [&](const auto& option) { return option.value == group.activationValue; })) return fail();
        }
      }
      return true;
    }
    bool validateSpringResponseGroups(const std::vector<ManifestField>& fields,std::string& error) {
      for(const auto& anchor:fields) {
        if(!anchor.springResponse)continue;
        for(const auto& key:anchor.springResponse->keys) {
          const auto field=std::ranges::find(fields,key,&ManifestField::key);
          if(field==fields.end() || (field->type!=ManifestFieldType::Double && field->type!=ManifestFieldType::Int)) {
            error="setting '"+anchor.key+"' spring_response references a missing or nonnumeric field";return false;
          }
        }
      }
      return true;
    }

    std::optional<ManifestField>
    parseField(const toml::table& field, std::uint32_t pluginApiVersion, std::string& error) {
      ManifestField out;
      out.key = tableString(field, "key");
      if (out.key.empty()) {
        return out;
      }
      out.type = parseFieldType(tableString(field, "type", "string"));
      if (out.type == ManifestFieldType::StringMap && pluginApiVersion < kStringMapSettingPluginApiVersion) {
        error = "setting '"
            + out.key
            + "' type 'string_map' requires plugin_api >= "
            + std::to_string(kStringMapSettingPluginApiVersion);
        return std::nullopt;
      }
      if (field.contains("label")) {
        error = "setting '" + out.key + "' uses 'label'; use 'label_key' that points to translation key instead";
        return std::nullopt;
      }
      if (field.contains("description")) {
        error = "setting '"
            + out.key
            + "' uses 'description'; use 'description_key' that points to translation key instead";
        return std::nullopt;
      }
      out.labelKey = tableString(field, "label_key");
      if (out.labelKey.empty()) {
        error = "setting '" + out.key + "' is missing 'label_key'";
        return std::nullopt;
      }
      out.descriptionKey = tableString(field, "description_key");
      out.advanced = tableBool(field, "advanced", false);
      out.optionsFrom = tableString(field, "options_from");
      if (!parseFieldDefault(field, out, error)) {
        return std::nullopt;
      }
      if (!parseFieldNumberOptions(field, out, error)) {
        return std::nullopt;
      }
      if (!parseFieldOptions(field, out, error)) {
        return std::nullopt;
      }
      if (!parseCurveGroup(field, out, error)) return std::nullopt;
      if (!parseSpringResponseGroup(field, out, error)) return std::nullopt;
      parseFieldExtensions(field, out);
      parseFieldVisibility(field, out);
      return out;
    }

    // Entry-level [[<entry>.setting]] is only honored for kinds that have a
    // settings editor: bar widgets and desktop widgets edit per-instance, panels
    // edit from the plugin page. Launcher providers, shortcuts, and services are
    // singletons with no settings surface; use a plugin-level [[setting]] instead.
    constexpr bool entryKindSupportsSettings(PluginEntryKind kind) {
      switch (kind) {
      case PluginEntryKind::Widget:
      case PluginEntryKind::DesktopWidget:
      case PluginEntryKind::Panel:
        return true;
      default:
        return false;
      }
    }

    bool routedOwnershipTypeMatches(ManifestFieldType type, const toml::node& value) {
      switch (type) {
      case ManifestFieldType::Bool:
        return value.is_boolean();
      case ManifestFieldType::Int:
        return value.is_integer();
      case ManifestFieldType::Double:
        return value.is_floating_point();
      case ManifestFieldType::String:
      case ManifestFieldType::File:
      case ManifestFieldType::Folder:
      case ManifestFieldType::Glyph:
      case ManifestFieldType::Select:
      case ManifestFieldType::Color:
        return value.is_string();
      case ManifestFieldType::StringList:
      case ManifestFieldType::StringMap:
        return false;
      }
      return false;
    }

    bool parseEntries(
        const toml::table& root, PluginEntryKind kind, std::string_view tableName, PluginManifest& manifest,
        std::string& error
    ) {
      const auto* entries = root[tableName].as_array();
      if (entries == nullptr) {
        return true;
      }
      for (const auto& node : *entries) {
        const auto* entryTable = node.as_table();
        if (entryTable == nullptr) {
          continue;
        }
        PluginEntry entry;
        entry.kind = kind;
        entry.id = tableString(*entryTable, "id");
        entry.entry = tableString(*entryTable, "entry");
        if (entry.id.empty()) {
          continue;
        }
        if (const auto* settings = (*entryTable)["setting"].as_array()) {
          if (!entryKindSupportsSettings(kind)) {
            error = "entry '"
                + entry.id
                + "' of kind '"
                + std::string(tableName)
                + "' declares [["
                + std::string(tableName)
                + ".setting]], but entry-level settings are only supported for widget, desktop_widget, and panel "
                  "entries; move it to a plugin-level [[setting]]";
            return false;
          }
          for (const auto& settingNode : *settings) {
            if (const auto* settingTable = settingNode.as_table()) {
              auto field = parseField(*settingTable, manifest.pluginApiVersion, error);
              if (!field.has_value()) {
                return false;
              }
              if (field->curve) {
                error = "curve groups require plugin-level settings";
                return false;
              }
              if (!field->key.empty()) {
                entry.settings.push_back(std::move(*field));
              }
            }
          }
        }
        if (kind == PluginEntryKind::Panel) {
          // width/height: absent = host default, positive number = logical px,
          // the literal string "fill" = span the output's available extent on
          // that axis. Anything else is a manifest error, never a default.
          const auto parsePanelExtent = [&](const char* key, double& outSize, bool& outFill) -> bool {
            const auto extentNode = (*entryTable)[key];
            if (!extentNode) {
              return true;
            }
            if (const auto* str = extentNode.as_string()) {
              if (str->get() == "fill") {
                outFill = true;
                return true;
              }
            } else if (
                const auto number = tableNumber(*entryTable, key);
                number.has_value() && std::isfinite(*number) && *number > 0.0
            ) {
              outSize = *number;
              return true;
            }
            error = "panel entry '" + entry.id + "': " + key + " must be a positive number or \"fill\"";
            return false;
          };
          if (!parsePanelExtent("width", entry.panelWidth, entry.panelWidthFill)
              || !parsePanelExtent("height", entry.panelHeight, entry.panelHeightFill)) {
            return false;
          }
          if (const std::string placement = tableString(*entryTable, "placement"); !placement.empty()) {
            entry.panelPlacementDefault = placement;
          }
          if ((entry.panelWidthFill || entry.panelHeightFill) && entry.panelPlacementDefault != "floating") {
            error = "panel entry '" + entry.id + R"(': width/height "fill" requires placement = "floating")";
            return false;
          }
          if (const std::string position = tableString(*entryTable, "position"); !position.empty()) {
            entry.panelPositionDefault = position;
          }
          if (const auto* openNearClick = (*entryTable)["open_near_click"].as_boolean()) {
            entry.panelOpenNearClickDefault = openNearClick->get();
          }
          if ((*entryTable)["layer"]) {
            if (manifest.pluginApiVersion < kPanelLayerPluginApiVersion) {
              error = "panel entry '"
                  + entry.id
                  + "': layer requires plugin_api >= "
                  + std::to_string(kPanelLayerPluginApiVersion);
              return false;
            }
            const auto* layer = (*entryTable)["layer"].as_string();
            if (layer == nullptr || !isValidPanelLayer(layer->get())) {
              error = "panel entry '" + entry.id + R"(': layer must be "top" or "overlay")";
              return false;
            }
            entry.panelLayerDefault = layer->get();
          }
          if ((*entryTable)["decorated"]) {
            const auto* decorated=(*entryTable)["decorated"].as_boolean();
            if (decorated==nullptr) { error="panel entry '"+entry.id+"': decorated must be a bool";return false; }
            entry.panelDecorated=decorated->get();
          }
          if ((*entryTable)["dismiss_on_outside_click"]) {
            if (manifest.pluginApiVersion < kPanelDismissOnOutsideClickPluginApiVersion) {
              error = "panel entry '"
                  + entry.id
                  + "': dismiss_on_outside_click requires plugin_api >= "
                  + std::to_string(kPanelDismissOnOutsideClickPluginApiVersion);
              return false;
            }
            if (const auto* dismissOutside = (*entryTable)["dismiss_on_outside_click"].as_boolean()) {
              entry.panelDismissOnOutsideClick = dismissOutside->get();
            } else {
              error = "panel entry '" + entry.id + "': dismiss_on_outside_click must be a bool";
              return false;
            }
          }
          if ((*entryTable)["keyboard_focus"]) {
            if (manifest.pluginApiVersion < kPanelKeyboardFocusPluginApiVersion) {
              error = "panel entry '"
                  + entry.id
                  + "': keyboard_focus requires plugin_api >= "
                  + std::to_string(kPanelKeyboardFocusPluginApiVersion);
              return false;
            }
            const auto* keyboardFocus = (*entryTable)["keyboard_focus"].as_string();
            if (keyboardFocus == nullptr || !isValidPanelKeyboardFocus(keyboardFocus->get())) {
              error = "panel entry '" + entry.id + R"(': keyboard_focus must be "on_demand", "exclusive" or "none")";
              return false;
            }
            entry.panelKeyboardFocus = keyboardFocus->get();
            // Outside-click dismissal is served either by the click shield (which
            // swallows the click meant for the app below) or by a compositor focus
            // grab (which takes keyboard focus). Neither is compatible with a panel
            // that must never touch focus.
            if (entry.panelKeyboardFocus == "none" && entry.panelDismissOnOutsideClick) {
              error = "panel entry '"
                  + entry.id
                  + R"(': keyboard_focus = "none" requires dismiss_on_outside_click = false)";
              return false;
            }
          }
          if ((*entryTable)["persistent"]) {
            if (manifest.pluginApiVersion < kPersistentPanelPluginApiVersion) {
              error = "panel entry '"
                  + entry.id
                  + "': persistent requires plugin_api >= "
                  + std::to_string(kPersistentPanelPluginApiVersion);
              return false;
            }
            const auto* persistent = (*entryTable)["persistent"].as_boolean();
            if (persistent == nullptr) {
              error = "panel entry '" + entry.id + "': persistent must be a bool";
              return false;
            }
            entry.panelPersistent = persistent->get();
          }
          if (entry.panelPersistent) {
            // A persistent panel has neither click shield nor focus grab.
            if (entry.panelDismissOnOutsideClick) {
              error = "panel entry '" + entry.id + "': persistent = true requires dismiss_on_outside_click = false";
              return false;
            }
            // Exclusive keyboard focus on a surface that is never dismissed would
            // hold the keyboard away from every other window for good.
            if (entry.panelKeyboardFocus == "exclusive") {
              error = "panel entry '"
                  + entry.id
                  + R"(': persistent = true is incompatible with keyboard_focus = "exclusive")";
              return false;
            }
            // Attached placement is resolved pre-commit against a live bar, which a
            // panel outside the active-panel slot has no relationship to.
            if (entry.panelPlacementDefault == "attached") {
              error = "panel entry '" + entry.id + R"(': persistent = true requires placement = "floating")";
              return false;
            }
          }
          if ((*entryTable)["directional_navigation"]) {
            const auto* enabled = (*entryTable)["directional_navigation"].as_boolean();
            if (enabled == nullptr) { error = "directional_navigation must be a bool"; return false; }
            entry.panelDirectionalNavigation = enabled->get();
          }
          if ((*entryTable)["capture_keys"]) {
            if (manifest.pluginApiVersion < kPanelCaptureKeysPluginApiVersion) {
              error = "panel entry '"
                  + entry.id
                  + "': capture_keys requires plugin_api >= "
                  + std::to_string(kPanelCaptureKeysPluginApiVersion);
              return false;
            }
            if ((*entryTable)["capture_keys"].as_array() == nullptr) {
              error = "panel entry '" + entry.id + "': capture_keys must be an array of key chord strings";
              return false;
            }
            entry.panelCaptureKeys = tableStringArray(*entryTable, "capture_keys");
            for (const std::string& spec : entry.panelCaptureKeys) {
              // parseKeyChordSpec throws on a Super-family modifier, which belongs to the
              // compositor rather than to a panel.
              bool parsed = false;
              try {
                parsed = parseKeyChordSpec(spec).has_value();
              } catch (const std::exception& e) {
                error = "panel entry '" + entry.id + "': capture_keys entry '" + spec + "': " + e.what();
                return false;
              }
              if (!parsed) {
                error = "panel entry '" + entry.id + "': capture_keys entry '" + spec + "' is not a valid key chord";
                return false;
              }
            }
            // A panel that never takes focus never receives a key to capture.
            if (entry.panelKeyboardFocus == "none") {
              error =
                  "panel entry '" + entry.id + R"(': capture_keys requires keyboard_focus "on_demand" or "exclusive")";
              return false;
            }
          }
          injectStandardPanelShellSettings(entry);
        }
        if (kind == PluginEntryKind::Widget) {
          if ((*entryTable)["actions"]) {
            if (manifest.pluginApiVersion < kWidgetGestureActionsPluginApiVersion) {
              error = "widget entry '"
                  + entry.id
                  + "': actions requires plugin_api >= "
                  + std::to_string(kWidgetGestureActionsPluginApiVersion);
              return false;
            }
            if ((*entryTable)["actions"].as_table() == nullptr) {
              error = "widget entry '" + entry.id + "': actions must be a table of gesture bindings";
              return false;
            }
          }
          if (const auto* actionsTable = (*entryTable)["actions"].as_table()) {
            for (const auto& [gestureKey, actionNode] : *actionsTable) {
              const auto action = actionNode.value<std::string>();
              if (!action.has_value()) {
                error =
                    "widget entry '" + entry.id + "': actions." + std::string(gestureKey.str()) + " must be a string";
                return false;
              }
              entry.widgetActions.emplace_back(std::string(gestureKey.str()), *action);
            }
          }
        }
        if (kind == PluginEntryKind::LauncherProvider) {
          entry.launcherPrefix = tableString(*entryTable, "prefix");
          entry.launcherGlyph = tableString(*entryTable, "glyph");
          entry.launcherGlobalSearch = tableBool(*entryTable, "include_in_global_search", false);
          entry.launcherDebounceMs =
              std::max(0, static_cast<int>(tableNumber(*entryTable, "debounce_ms").value_or(0.0)));
          if (const auto* cats = (*entryTable)["category"].as_array()) {
            for (const auto& catNode : *cats) {
              if (const auto* catTable = catNode.as_table()) {
                ManifestLauncherCategory cat;
                cat.label = tableString(*catTable, "label");
                cat.glyph = tableString(*catTable, "glyph");
                if (!cat.label.empty()) {
                  entry.launcherCategories.push_back(std::move(cat));
                }
              }
            }
          }
        }
        manifest.entries.push_back(std::move(entry));
      }
      return true;
    }

  } // namespace

  WidgetSettingValue ManifestField::defaultValue() const {
    switch (type) {
    case ManifestFieldType::Bool:
      return WidgetSettingValue{boolDefault};
    case ManifestFieldType::Int:
      return WidgetSettingValue{static_cast<std::int64_t>(numberDefault)};
    case ManifestFieldType::Double:
      return WidgetSettingValue{numberDefault};
    case ManifestFieldType::StringList:
      return WidgetSettingValue{stringListDefault};
    case ManifestFieldType::StringMap:
      return WidgetSettingValue{stringMapDefault};
    default:
      return WidgetSettingValue{stringDefault};
    }
  }

  const PluginEntry* PluginManifest::findEntry(std::string_view entryId) const {
    const auto it = std::ranges::find(entries, entryId, &PluginEntry::id);
    return it != entries.end() ? &*it : nullptr;
  }

  std::string_view pluginEntryTableName(PluginEntryKind kind) {
    for (const auto& [k, name] : kEntryKinds) {
      if (k == kind) {
        return name;
      }
    }
    return {};
  }
  bool isValidPluginVersion(std::string_view version) {
    std::size_t componentStart = 0;
    for (std::size_t componentIndex = 0; componentIndex < 3; ++componentIndex) {
      const bool isLast = componentIndex == 2;
      const std::size_t separator = version.find('.', componentStart);
      if ((isLast && separator != std::string_view::npos) || (!isLast && separator == std::string_view::npos)) {
        return false;
      }

      const std::string_view component =
          version.substr(componentStart, isLast ? std::string_view::npos : separator - componentStart);
      if (component.empty()
          || (component.size() > 1 && component.front() == '0')
          || !std::ranges::all_of(component, [](char ch) { return ch >= '0' && ch <= '9'; })) {
        return false;
      }
      if (!isLast) {
        componentStart = separator + 1;
      }
    }
    return true;
  }

  std::unordered_map<std::string, WidgetSettingValue>
  seedEntrySettings(const PluginEntry& entry, const std::unordered_map<std::string, WidgetSettingValue>& overrides) {
    std::unordered_map<std::string, WidgetSettingValue> seeded;
    seeded.reserve(entry.settings.size());
    for (const ManifestField& field : entry.settings) {
      if (const auto it = overrides.find(field.key); it != overrides.end()) {
        seeded.emplace(field.key, it->second);
      } else {
        seeded.emplace(field.key, field.defaultValue());
      }
    }
    return seeded;
  }

  std::optional<PluginManifest> parsePluginManifest(const std::filesystem::path& manifestPath, std::string* error) {
    const auto fail = [error](std::string message) -> std::optional<PluginManifest> {
      if (error != nullptr) {
        *error = std::move(message);
      }
      return std::nullopt;
    };

    toml::table root;
    try {
      root = toml::parse_file(manifestPath.string());
    } catch (const toml::parse_error& e) {
      return fail(std::string("parse error: ") + e.description().data());
    }

    PluginManifest manifest;
    manifest.id = tableString(root, "id");
    if (manifest.id.empty()) {
      return fail("missing mandatory key 'id'");
    }
    if (!isValidPluginId(manifest.id)) {
      return fail("invalid plugin id '" + manifest.id + "' (expected author/plugin)");
    }
    manifest.name = tableString(root, "name");
    if (manifest.name.empty()) {
      return fail("missing mandatory key 'name'");
    }
    if (!root.contains("version")) {
      return fail("missing mandatory key 'version'");
    }
    manifest.version = tableString(root, "version");
    if (!isValidPluginVersion(manifest.version)) {
      return fail("invalid 'version' (expected MAJOR.MINOR.PATCH)");
    }
    if (!root.contains("plugin_api")) {
      return fail("missing mandatory key 'plugin_api'");
    }
    const auto pluginApiVersion = root["plugin_api"].value<std::int64_t>();
    if (!pluginApiVersion.has_value()
        || *pluginApiVersion <= 0
        || static_cast<std::uint64_t>(*pluginApiVersion) > std::numeric_limits<std::uint32_t>::max()) {
      return fail("invalid 'plugin_api' (expected a positive integer)");
    }
    manifest.pluginApiVersion = static_cast<std::uint32_t>(*pluginApiVersion);

    manifest.author = tableString(root, "author");
    manifest.license = tableString(root, "license", "MIT");
    manifest.deprecated = tableBool(root, "deprecated", false);
    manifest.icon = tableString(root, "icon");
    manifest.description = tableString(root, "description");
    manifest.tags = tableStringArray(root, "tags");
    manifest.dependencies = tableStringArray(root, "dependencies");

    std::string manifestError;
    for (const auto& [kind, tableName] : kEntryKinds) {
      if (!parseEntries(root, kind, tableName, manifest, manifestError)) {
        return fail(manifestError);
      }
    }

    if (const auto* settings = root["setting"].as_array()) {
      for (const auto& node : *settings) {
        if (const auto* settingTable = node.as_table()) {
          auto field = parseField(*settingTable, manifest.pluginApiVersion, manifestError);
          if (!field.has_value()) {
            return fail(manifestError);
          }
          if (!field->key.empty()) {
            manifest.settings.push_back(std::move(*field));
          }
        }
      }
    }

    manifest.settingsTabs = tableString(root, "settings_tabs");
    if (!manifest.settingsTabs.empty()) {
      const auto it = std::ranges::find(manifest.settings, manifest.settingsTabs, &ManifestField::key);
      if (it == manifest.settings.end() || it->type != ManifestFieldType::Select || it->options.empty())
        return fail("settings_tabs must name a nonempty plugin-level select setting");
    }

    manifest.presetActions = tableString(root, "preset_actions");
    if (const auto* command = root.get("preset_command")) {
      const auto* args = command->as_array();
      if (!args || args->empty() || args->size() > 16)
        return fail("preset_command must contain between 1 and 16 argv strings");
      std::size_t total = 0;
      for (const auto& node : *args) {
        const auto arg = node.value<std::string>();
        if (!arg || arg->empty() || arg->size() > 1024 || arg->find('\0') != std::string::npos)
          return fail("preset_command arguments must be nonempty strings of at most 1024 bytes without NUL");
        total += arg->size();
        if (total > 4096) return fail("preset_command exceeds 4096 bytes");
        manifest.presetCommand.push_back(*arg);
      }
    }
    manifest.presetSelection = tableString(root, "preset_selection");
    if (root.contains("preset_selection") && manifest.presetSelection.empty())
      return fail("preset_selection must name a plugin-level select setting");
    if (!manifest.presetSelection.empty()) {
      const auto selection = std::ranges::find(manifest.settings, manifest.presetSelection, &ManifestField::key);
      if (selection == manifest.settings.end() || selection->type != ManifestFieldType::Select)
        return fail("preset_selection must name a plugin-level select setting");
      if (manifest.presetCommand.empty()) return fail("preset_selection requires preset_command");
    }
    if (!manifest.presetActions.empty()) {
      const auto action = std::ranges::find(manifest.settings, manifest.presetActions, &ManifestField::key);
      if (action == manifest.settings.end() || action->type != ManifestFieldType::Select)
        return fail("preset_actions must name a plugin-level select setting");
    }
    manifest.presetSections = tableStringArray(root, "preset_sections");
    manifest.presetTabs = tableStringArray(root, "preset_tabs");
    if (manifest.presetSections.size() > 32 || manifest.presetTabs.size() > 32)
      return fail("Too many preset sections");
    if (const auto* targets = root["settings_tab_targets"].as_table()) {
      for (const auto& [key, node] : *targets) {
        const auto target = node.value<std::string>();
        if (!target || target->empty()) return fail("settings_tab_targets requires section names");
        manifest.settingsTabTargets.emplace(std::string(key.str()), *target);
      }
    }

    if (const auto* rules = root["settings_ownership"].as_array()) {
      if (rules->size() > 64) return fail("Too many settings ownership rules");
      for (const auto& node : *rules) {
        const auto* rule = node.as_table();
        if (!rule) return fail("Settings ownership must be a table");
        auto path = tableStringArray(*rule, "path");
        auto when = tableString(*rule, "when");
        const auto toggle = std::ranges::find(manifest.settings, when, &ManifestField::key);
        if (path.empty() || path.size() > 8 || toggle == manifest.settings.end() || toggle->type != ManifestFieldType::Bool)
          return fail("Settings ownership requires a path and a boolean plugin setting");
        auto setting = tableString(*rule, "setting");
        if (rule->contains("setting")) {
          const auto source = std::ranges::find(manifest.settings, setting, &ManifestField::key);
          if (setting.empty() || source == manifest.settings.end() || std::ranges::find(path, "*") != path.end())
            return fail("Routed ownership requires an exact native path and a plugin setting");
          const auto defaults = config_export::serialize(Config{});
          const toml::node* value = &defaults;
          for (const auto& key : path) value = value && value->is_table() ? value->as_table()->get(key) : nullptr;
          if (!value || path[0] == "plugin_settings" || !routedOwnershipTypeMatches(source->type, *value))
            return fail("Routed ownership path and plugin setting must have the same supported scalar type");
        }
        if (std::ranges::any_of(manifest.settingsOwnership, [&](const auto& existing) { return existing.path == path; }))
          return fail("Duplicate settings ownership path");
        manifest.settingsOwnership.push_back({std::move(path), std::move(when), std::move(setting)});
      }
    }
    for (const auto& field : manifest.settings) {
      if (field.optionsFrom.empty()) continue;
      const auto source = std::ranges::find(manifest.settings, field.optionsFrom, &ManifestField::key);
      if (field.type != ManifestFieldType::Select || source == manifest.settings.end() || source->type != ManifestFieldType::StringList)
        return fail("options_from requires a plugin-level string-list setting");
    }

    std::string curveError;
    if (!validateCurveGroups(manifest.settings, curveError)) return fail(curveError);
    if (!validateSpringResponseGroups(manifest.settings, curveError)) return fail(curveError);

    std::unordered_set<std::string> seenEntryIds;
    for (const auto& entry : manifest.entries) {
      if (!seenEntryIds.insert(entry.id).second) {
        return fail("duplicate entry id '" + entry.id + "'");
      }
    }

    std::unordered_set<std::string> pluginLevelKeys;
    for (const auto& field : manifest.settings) {
      pluginLevelKeys.insert(field.key);
    }
    for (const auto& entry : manifest.entries) {
      for (const auto& field : entry.settings) {
        if (pluginLevelKeys.contains(field.key)) {
          kLog.warn(
              "plugin '{}' entry '{}' setting '{}' shadows a plugin-level setting; entry value wins", manifest.id,
              entry.id, field.key
          );
        }
      }
    }

    return manifest;
  }

  void mergePluginSettings(
      const PluginManifest& manifest, const std::unordered_map<std::string, WidgetSettingValue>& pluginOverrides,
      std::unordered_map<std::string, WidgetSettingValue>& seeded
  ) {
    for (const ManifestField& field : manifest.settings) {
      if (seeded.contains(field.key)) {
        continue; // entry-level setting declared the same key — entry wins
      }
      const auto it = pluginOverrides.find(field.key);
      seeded.emplace(field.key, it != pluginOverrides.end() ? it->second : field.defaultValue());
    }
  }

  bool settingsEqual(
      const std::unordered_map<std::string, WidgetSettingValue>& a,
      const std::unordered_map<std::string, WidgetSettingValue>& b
  ) {
    if (a.size() != b.size()) {
      return false;
    }
    for (const auto& [key, value] : a) {
      const auto it = b.find(key);
      if (it == b.end() || !valueEqual(value, it->second)) {
        return false;
      }
    }
    return true;
  }

} // namespace scripting
