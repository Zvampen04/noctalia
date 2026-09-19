#include "shell/settings/bar_widget_editor.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "core/files/directory_scanner.h"
#include "cursor-shape-v1-client-protocol.h"
#include "i18n/i18n.h"
#include "render/scene/node.h"
#include "shell/bar/widget_gesture.h"
#include "shell/bar/widget_gesture_defaults.h"
#include "shell/bar/widget_action.h"
#include "shell/bar/bar_material_target.h"
#include "shell/settings/color_spec_picker.h"
#include "shell/settings/font_weight_catalog.h"
#include "shell/settings/path_browse.h"
#include "shell/settings/settings_content.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/collapsible.h"
#include "ui/dialogs/file_dialog.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace settings {

  // Defined in settings_content_common.cpp (header not included here to avoid a makeLabel overload clash).
  [[nodiscard]] std::string formatSliderValue(double value, bool integerValue);
  [[nodiscard]] std::optional<double> parseDoubleInput(std::string_view text);

  namespace {

    struct LaneWidgetDragState {
      bool active = false;
      bool moved = false;
      float startLocalX = 0.0F;
      float startLocalY = 0.0F;
      float lastLocalX = 0.0F;
      float lastLocalY = 0.0F;
      std::optional<std::size_t> targetZoneIndex;
      std::optional<std::size_t> targetInsertionIndex;
      // Set when hovering over the middle of another loose widget: dropping forms a new group with it.
      std::optional<std::size_t> combineZoneIndex;
      std::optional<std::size_t> combineItemIndex;
      // The card currently highlighted as a combine target (so it can be reset).
      std::optional<std::size_t> highlightZoneIndex;
      std::optional<std::size_t> highlightItemIndex;
    };

    // A drop zone is either a bar lane (entries = widget refs / group tokens) or a group's member list.
    // Both lanes and group containers register as zones so widgets can be dragged into and out of groups.
    struct DropZone {
      bool isGroup = false;
      std::vector<std::string> lanePath; // when !isGroup
      std::string groupId;               // when isGroup
      std::vector<std::string> items;    // lane entries or group members (snapshot)
      Flex* container = nullptr;
      Box* indicator = nullptr;
      std::shared_ptr<std::vector<Flex*>> itemNodes;
    };

    std::unique_ptr<Label> makeLabel(
        std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal
    ) {
      return ui::label({
          .text = std::string(text),
          .fontSize = fontSize,
          .fontWeight = fontWeight,
          .color = color,
      });
    }

    std::unique_ptr<Glyph> makeGlyph(std::string_view name, float glyphSize, const ColorSpec& color) {
      return ui::glyph({
          .glyph = std::string(name),
          .glyphSize = glyphSize,
          .color = color,
      });
    }

    std::unique_ptr<Node> makeMiniSectionHeader(std::string_view title, float scale, bool withSeparator = true) {
      auto header = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .configure = [scale](Flex& flex) { flex.setPadding(Style::spaceSm * scale, 0.0F, 0.0F, 0.0F); },
      });
      if (withSeparator) {
        header->addChild(ui::separator());
      }
      header->addChild(
          makeLabel(title, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold)
      );
      return header;
    }

    // One row per bindable gesture: a picker over Default / Disabled / every command / a free-form
    // shell command, plus an argument field when the choice takes one.
    // One row per bindable gesture, built by the shared factory so this matches every other
    // gesture-binding surface.
    void addGestureActionRows(
        Flex& panel, const BarWidgetEditorContext& ctx, const SettingEntry& entry,
        const WidgetSettingStringMap& defaults, const WidgetSettingStringMap& configured,
        noctalia::bar::GestureMask reserved
    ) {
      for (const auto gesture : noctalia::bar::allGestures()) {
        if (reserved.contains(gesture)) {
          continue;
        }
        const std::string key(noctalia::bar::gestureConfigKey(gesture));
        std::vector<std::string> path = entry.path;
        path.push_back(key);

        const auto configuredIt = configured.find(key);
        const auto defaultIt = defaults.find(key);

        SettingEntry rowEntry = entry;
        rowEntry.path = path;
        rowEntry.title = i18n::tr(std::string(noctalia::bar::gestureLabelKey(gesture)));
        rowEntry.subtitle.clear();

        GestureActionSetting setting{
            .gestureKey = key,
            .configured = configuredIt != configured.end() ? configuredIt->second : std::string{},
            .defaultAction = defaultIt != defaults.end() ? defaultIt->second : std::string{},
        };
        ctx.makeRow(panel, rowEntry, ctx.makeGestureActionRow(setting, rowEntry.title, path));
      }
    }

    std::string widgetSettingGroupTitle(std::string_view groupKey) {
      return i18n::tr("settings.entities.widget.settings.groups." + std::string(groupKey));
    }

    constexpr std::string_view kGestureActionsGroup = "actions";

    // The actions group is long (one row per bindable gesture) and most widgets never need it, so it
    // starts folded. The open state lives on the settings window keyed by widget name: editing a
    // binding rebuilds the scene, and a local flag would fold the group back up on every edit.
    std::unique_ptr<Node> makeGestureActionsSection(
        const BarWidgetEditorContext& ctx, const std::string& widgetName, std::unique_ptr<Node> body, bool withSeparator
    ) {
      // Same padding and gap as makeMiniSectionHeader, so this section sits like every other one.
      auto section = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * ctx.scale,
          .configure = [scale = ctx.scale](Flex& flex) { flex.setPadding(Style::spaceSm * scale, 0.0F, 0.0F, 0.0F); },
      });
      if (withSeparator) {
        section->addChild(ui::separator());
      }

      auto collapsible = std::make_unique<Collapsible>();
      collapsible->setScale(ctx.scale);
      // Flush left, matching the plain group headers above it.
      collapsible->setHeaderPadding(0.0F, 0.0F);
      collapsible->setHeader(makeLabel(
          widgetSettingGroupTitle(kGestureActionsGroup), Style::fontSizeCaption * ctx.scale,
          colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold
      ));
      collapsible->setBody(std::move(body));
      collapsible->setExpandedImmediate(ctx.actionsExpandedFor == widgetName);
      collapsible->setOnToggle([expandedFor = &ctx.actionsExpandedFor, widgetName](bool value) {
        *expandedFor = value ? widgetName : std::string{};
      });
      section->addChild(std::move(collapsible));
      return section;
    }

    std::unique_ptr<Node> makePathBrowseControl(
        const BarWidgetEditorContext& ctx, std::vector<std::string> path, std::string currentValue, std::string glyph,
        FileDialogOptions options, PathBrowseKind kind, std::string dialogStartValue = {}
    ) {
      if (dialogStartValue.empty()) {
        dialogStartValue = currentValue;
      }

      auto textNode = ctx.makeText(currentValue, {}, path);
      return ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceSm * ctx.scale,
          },
          std::move(textNode),
          ui::button({
              .glyph = std::move(glyph),
              .glyphSize = Style::fontSizeBody * ctx.scale,
              .variant = ButtonVariant::Default,
              .minWidth = Style::controlHeight * ctx.scale,
              .minHeight = Style::controlHeight * ctx.scale,
              .paddingV = Style::spaceXs * ctx.scale,
              .paddingH = Style::spaceSm * ctx.scale,
              .radius = Style::scaledRadiusMd(ctx.scale),
              .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path = std::move(path),
                          options = std::move(options), kind, dialogStartValue = std::move(dialogStartValue)]() {
                FileDialogOptions dialogOptions = options;
                applyPathDialogStartValue(dialogOptions, dialogStartValue, kind);
                (void)FileDialog::open(
                    std::move(dialogOptions),
                    [setOverride, requestRebuild, path](std::optional<std::filesystem::path> picked) {
                      if (!picked.has_value()) {
                        return;
                      }
                      setOverride(path, picked->string());
                      if (requestRebuild) {
                        requestRebuild();
                      }
                    }
                );
              },
          })
      );
    }

    std::string pathKey(const std::vector<std::string>& path) {
      std::string out;
      for (const auto& part : path) {
        if (!out.empty()) {
          out.push_back('.');
        }
        out += part;
      }
      return out;
    }

    std::vector<std::string> pathWithLastSegment(std::vector<std::string> path, std::string segment) {
      if (!path.empty()) {
        path.back() = std::move(segment);
      }
      return path;
    }

    std::string laneLabel(std::string_view lane) {
      if (lane == "start") {
        return i18n::tr("settings.entities.widget.lanes.start");
      }
      if (lane == "center") {
        return i18n::tr("settings.entities.widget.lanes.center");
      }
      if (lane == "end") {
        return i18n::tr("settings.entities.widget.lanes.end");
      }
      return std::string(lane);
    }

    std::vector<std::string> barWidgetItemsForPath(const Config& cfg, const std::vector<std::string>& path) {
      if (!isBarWidgetListPath(path) || path.size() < 3) {
        return {};
      }

      const auto* bar = findBar(cfg, path[1]);
      if (bar == nullptr) {
        return {};
      }

      const auto& lane = path.back();
      if (path.size() >= 5 && path[2] == "monitor") {
        const auto* ovr = findMonitorOverride(*bar, path[3]);
        if (ovr != nullptr) {
          if (lane == "start") {
            return ovr->startWidgets.value_or(bar->startWidgets);
          }
          if (lane == "center") {
            return ovr->centerWidgets.value_or(bar->centerWidgets);
          }
          if (lane == "end") {
            return ovr->endWidgets.value_or(bar->endWidgets);
          }
        }
      }

      if (lane == "start") {
        return bar->startWidgets;
      }
      if (lane == "center") {
        return bar->centerWidgets;
      }
      if (lane == "end") {
        return bar->endWidgets;
      }
      return {};
    }

    std::unordered_map<std::string, std::string> barWidgetPlacementMap(const BarConfig* bar) {
      std::unordered_map<std::string, std::string> result;
      if (bar != nullptr) {
        for (const auto& placement : bar->widgetPlacements)
          result.insert_or_assign(placement.id, placement.widget);
      }
      return result;
    }

    std::pair<std::string, std::string> resolveBarWidgetEntry(
        const Config& cfg, const std::vector<std::string>& lanePath, std::string_view entry
    ) {
      const BarConfig* bar = lanePath.size() >= 2 ? findBar(cfg, lanePath[1]) : nullptr;
      const auto placements = barWidgetPlacementMap(bar);
      const auto resolved = noctalia::bar::resolveBarWidgetLaneEntry(entry, placements);
      return {std::string(resolved.widgetConfigName), std::string(resolved.placementId)};
    }

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>
    migrateBarWidgetPlacements(const Config& cfg, std::string_view barName) {
      const BarConfig* bar = findBar(cfg, barName);
      if (bar == nullptr) return {};
      auto placements = bar->widgetPlacements;
      std::unordered_map<std::string, std::string> placementMap = barWidgetPlacementMap(bar);
      std::unordered_set<std::string> occupied;
      for (const auto& placement : placements) occupied.insert(placement.id);
      std::size_t occurrence = 0;
      const auto migrate = [&](std::vector<std::string>& entries, std::string_view owner) {
        bool changed = false;
        for (auto& entry : entries) {
          if (isCapsuleGroupToken(entry)) continue;
          if (noctalia::bar::resolveBarWidgetLaneEntry(entry, placementMap).isPlacement()) continue;
          const std::string id = noctalia::bar::legacyBarWidgetPlacementId(
              barName, owner, entry, occurrence++, occupied
          );
          occupied.insert(id);
          placementMap.insert_or_assign(id, entry);
          placements.push_back({.id = id, .widget = entry});
          entry = noctalia::bar::makeBarWidgetPlacementToken(id);
          changed = true;
        }
        return changed;
      };

      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> batch;
      const auto migrateLane = [&](const std::vector<std::string>& source, std::string_view lane) {
        auto next = source;
        if (migrate(next, lane)) batch.emplace_back(
            std::vector<std::string>{"bar", std::string(barName), std::string(lane)}, std::move(next)
        );
      };
      migrateLane(bar->startWidgets, "start");
      migrateLane(bar->centerWidgets, "center");
      migrateLane(bar->endWidgets, "end");
      auto groups = bar->widgetCapsuleGroups;
      bool groupsChanged = false;
      for (auto& group : groups) groupsChanged |= migrate(group.members, "group." + group.id);
      if (groupsChanged) batch.emplace_back(
          std::vector<std::string>{"bar", std::string(barName), "capsule_group"}, std::move(groups)
      );
      auto sections = bar->sections;
      bool sectionsChanged = false;
      for (auto& section : sections) sectionsChanged |= migrate(section.widgets, "section." + section.id);
      if (sectionsChanged) batch.emplace_back(
          std::vector<std::string>{"bar", std::string(barName), "section"}, std::move(sections)
      );

      for (const auto& monitor : bar->monitorOverrides) {
        const std::vector<std::string> prefix{"bar", std::string(barName), "monitor", monitor.match};
        const auto migrateOptionalLane = [&](const std::optional<std::vector<std::string>>& source,
                                             std::string_view lane) {
          if (!source) return;
          auto next = *source;
          if (migrate(next, "monitor." + monitor.match + "." + std::string(lane))) {
            auto path = prefix;
            path.push_back(std::string(lane));
            batch.emplace_back(std::move(path), std::move(next));
          }
        };
        migrateOptionalLane(monitor.startWidgets, "start");
        migrateOptionalLane(monitor.centerWidgets, "center");
        migrateOptionalLane(monitor.endWidgets, "end");
        if (monitor.widgetCapsuleGroups) {
          auto next = *monitor.widgetCapsuleGroups;
          bool changed = false;
          for (auto& group : next)
            changed |= migrate(group.members, "monitor." + monitor.match + ".group." + group.id);
          if (changed) {
            auto path = prefix;
            path.push_back("capsule_group");
            batch.emplace_back(std::move(path), std::move(next));
          }
        }
        if (monitor.sectionsSpecified) {
          auto next = monitor.sections;
          bool changed = false;
          for (auto& section : next)
            changed |= migrate(section.widgets, "monitor." + monitor.match + ".section." + section.id);
          if (changed) {
            auto path = prefix;
            path.push_back("section");
            batch.emplace_back(std::move(path), std::move(next));
          }
        }
      }
      if (!batch.empty()) batch.insert(
          batch.begin(), {{"bar", std::string(barName), "widget_placement"}, std::move(placements)}
      );
      return batch;
    }

    std::size_t barWidgetPlacementReferenceCount(const BarConfig& bar, std::string_view placementId) {
      const std::string token = noctalia::bar::makeBarWidgetPlacementToken(placementId);
      std::size_t count = 0;
      const auto countIn = [&count, &token](const std::vector<std::string>& values) {
        count += static_cast<std::size_t>(std::ranges::count(values, token));
      };
      countIn(bar.startWidgets);
      countIn(bar.centerWidgets);
      countIn(bar.endWidgets);
      for (const auto& group : bar.widgetCapsuleGroups) countIn(group.members);
      for (const auto& section : bar.sections) countIn(section.widgets);
      for (const auto& monitor : bar.monitorOverrides) {
        if (monitor.startWidgets) countIn(*monitor.startWidgets);
        if (monitor.centerWidgets) countIn(*monitor.centerWidgets);
        if (monitor.endWidgets) countIn(*monitor.endWidgets);
        if (monitor.widgetCapsuleGroups)
          for (const auto& group : *monitor.widgetCapsuleGroups) countIn(group.members);
        if (monitor.sectionsSpecified)
          for (const auto& section : monitor.sections) countIn(section.widgets);
      }
      return count;
    }

    bool isMonitorWidgetListPath(const std::vector<std::string>& path) {
      return isBarWidgetListPath(path) && path.size() >= 5 && path[2] == "monitor";
    }

    bool monitorWidgetListHasExplicitValue(const Config& cfg, const std::vector<std::string>& path) {
      if (!isMonitorWidgetListPath(path)) {
        return true;
      }

      const auto* bar = findBar(cfg, path[1]);
      if (bar == nullptr) {
        return true;
      }
      const auto* ovr = findMonitorOverride(*bar, path[3]);
      if (ovr == nullptr) {
        return false;
      }

      const auto& lane = path.back();
      if (lane == "start") {
        return ovr->startWidgets.has_value();
      }
      if (lane == "center") {
        return ovr->centerWidgets.has_value();
      }
      if (lane == "end") {
        return ovr->endWidgets.has_value();
      }
      return true;
    }

    // Compact kind indicator used on lane cards in place of the text badge.
    std::string_view widgetBadgeGlyph(WidgetReferenceKind kind) {
      switch (kind) {
      case WidgetReferenceKind::BuiltIn:
        return "box";
      case WidgetReferenceKind::Named:
        return "tag";
      case WidgetReferenceKind::Plugin:
        return "puzzle";
      case WidgetReferenceKind::Unknown:
        return "help-circle";
      }
      return "help-circle";
    }

    ColorSpec widgetBadgeGlyphColor(WidgetReferenceKind kind) {
      switch (kind) {
      case WidgetReferenceKind::BuiltIn:
        return colorSpecFromRole(ColorRole::Primary);
      case WidgetReferenceKind::Named:
      case WidgetReferenceKind::Plugin:
        return colorSpecFromRole(ColorRole::Secondary);
      case WidgetReferenceKind::Unknown:
        return colorSpecFromRole(ColorRole::Error);
      }
      return colorSpecFromRole(ColorRole::OnSurfaceVariant);
    }

    void collectWidgetReferenceNames(const std::vector<std::string>& widgets, std::unordered_set<std::string>& seen) {
      for (const auto& widget : widgets) {
        seen.insert(widget);
      }
    }

    bool widgetReferenceNameExists(const Config& cfg, std::string_view name) {
      const std::string key(name);
      if (isBuiltInWidgetType(name) || cfg.widgets.contains(key)) {
        return true;
      }

      std::unordered_set<std::string> seen;
      for (const auto& bar : cfg.bars) {
        collectWidgetReferenceNames(bar.startWidgets, seen);
        collectWidgetReferenceNames(bar.centerWidgets, seen);
        collectWidgetReferenceNames(bar.endWidgets, seen);
        for (const auto& ovr : bar.monitorOverrides) {
          if (ovr.startWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.startWidgets, seen);
          }
          if (ovr.centerWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.centerWidgets, seen);
          }
          if (ovr.endWidgets.has_value()) {
            collectWidgetReferenceNames(*ovr.endWidgets, seen);
          }
        }
      }
      return seen.contains(key);
    }

    bool removeWidgetReference(std::vector<std::string>& items, std::string_view widgetName) {
      const auto oldSize = items.size();
      const std::string key(widgetName);
      std::erase(items, key);
      return items.size() != oldSize;
    }

    void appendReferenceRemoval(
        std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& overrides, std::vector<std::string> path,
        std::vector<std::string> items, std::string_view widgetName
    ) {
      if (removeWidgetReference(items, widgetName)) {
        overrides.emplace_back(std::move(path), std::move(items));
      }
    }

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>
    widgetReferenceRemovalOverrides(const Config& cfg, std::string_view widgetName) {
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
      for (const auto& bar : cfg.bars) {
        appendReferenceRemoval(overrides, {"bar", bar.name, "start"}, bar.startWidgets, widgetName);
        appendReferenceRemoval(overrides, {"bar", bar.name, "center"}, bar.centerWidgets, widgetName);
        appendReferenceRemoval(overrides, {"bar", bar.name, "end"}, bar.endWidgets, widgetName);

        for (const auto& ovr : bar.monitorOverrides) {
          const std::vector<std::string> prefix = {"bar", bar.name, "monitor", ovr.match};
          if (ovr.startWidgets.has_value()) {
            appendReferenceRemoval(
                overrides, {prefix[0], prefix[1], prefix[2], prefix[3], "start"}, *ovr.startWidgets, widgetName
            );
          }
          if (ovr.centerWidgets.has_value()) {
            appendReferenceRemoval(
                overrides, {prefix[0], prefix[1], prefix[2], prefix[3], "center"}, *ovr.centerWidgets, widgetName
            );
          }
          if (ovr.endWidgets.has_value()) {
            appendReferenceRemoval(
                overrides, {prefix[0], prefix[1], prefix[2], prefix[3], "end"}, *ovr.endWidgets, widgetName
            );
          }
        }
      }
      return overrides;
    }

    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>
    widgetReferenceRenameOverrides(const Config& cfg, std::string_view oldName, std::string_view newName) {
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> overrides;
      for (const auto& bar : cfg.bars) {
        auto appendRename = [&](std::vector<std::string> path, std::vector<std::string> items) {
          bool changed = false;
          for (auto& item : items) {
            if (item == oldName) {
              item = std::string(newName);
              changed = true;
            }
          }
          if (changed) {
            overrides.emplace_back(std::move(path), std::move(items));
          }
        };

        appendRename({"bar", bar.name, "start"}, bar.startWidgets);
        appendRename({"bar", bar.name, "center"}, bar.centerWidgets);
        appendRename({"bar", bar.name, "end"}, bar.endWidgets);

        for (const auto& ovr : bar.monitorOverrides) {
          const std::vector<std::string> prefix = {"bar", bar.name, "monitor", ovr.match};
          if (ovr.startWidgets.has_value()) {
            appendRename({prefix[0], prefix[1], prefix[2], prefix[3], "start"}, *ovr.startWidgets);
          }
          if (ovr.centerWidgets.has_value()) {
            appendRename({prefix[0], prefix[1], prefix[2], prefix[3], "center"}, *ovr.centerWidgets);
          }
          if (ovr.endWidgets.has_value()) {
            appendRename({prefix[0], prefix[1], prefix[2], prefix[3], "end"}, *ovr.endWidgets);
          }
        }
      }
      return overrides;
    }

    bool isNamedWidgetInstance(const Config& cfg, std::string_view widgetName) {
      return cfg.widgets.contains(std::string(widgetName)) && !isBuiltInWidgetType(widgetName);
    }

    bool isGuiManagedNamedWidgetInstance(const BarWidgetEditorContext& ctx, std::string_view widgetName) {
      return isNamedWidgetInstance(ctx.config, widgetName)
          && ctx.configService != nullptr
          && ctx.configService->hasOverride({"widget", std::string(widgetName)});
    }

    bool widgetHasPlacementAfterLaneEdit(
        const Config& cfg, const std::vector<std::string>& editedLanePath,
        const std::vector<std::string>& editedLaneItems, std::string_view widgetName
    ) {
      const auto editedItems = [&](const std::vector<std::string>& path,
                                   const std::vector<std::string>& items) -> const std::vector<std::string>& {
        return path == editedLanePath ? editedLaneItems : items;
      };
      const auto scopeContains = [&](const std::vector<std::string>& start, const std::vector<std::string>& center,
                                     const std::vector<std::string>& end,
                                     const std::vector<BarCapsuleGroupStyle>& groups,
                                     const std::unordered_map<std::string, std::string>& placements) {
        const auto containsWidget = [&](const std::vector<std::string>& entries) {
          return std::ranges::any_of(entries, [&](const std::string& entry) {
            return noctalia::bar::resolveBarWidgetLaneEntry(entry, placements).widgetConfigName == widgetName;
          });
        };
        if (containsWidget(start) || containsWidget(center) || containsWidget(end)) {
          return true;
        }
        for (const auto& group : groups) {
          if (!containsWidget(group.members)) {
            continue;
          }
          const std::string token = makeCapsuleGroupToken(group.id);
          if (std::ranges::contains(start, token)
              || std::ranges::contains(center, token)
              || std::ranges::contains(end, token)) {
            return true;
          }
        }
        return false;
      };

      for (const auto& bar : cfg.bars) {
        const auto placements = barWidgetPlacementMap(&bar);
        const std::vector<std::string>& baseStart = editedItems({"bar", bar.name, "start"}, bar.startWidgets);
        const std::vector<std::string>& baseCenter = editedItems({"bar", bar.name, "center"}, bar.centerWidgets);
        const std::vector<std::string>& baseEnd = editedItems({"bar", bar.name, "end"}, bar.endWidgets);
        if (scopeContains(baseStart, baseCenter, baseEnd, bar.widgetCapsuleGroups, placements)
            || std::ranges::any_of(bar.sections, [&](const BarSectionConfig& section) {
                 return std::ranges::any_of(section.widgets, [&](const std::string& widget) {
                   return noctalia::bar::resolveBarWidgetLaneEntry(widget, placements).widgetConfigName == widgetName;
                 });
               })) {
          return true;
        }

        for (const auto& ovr : bar.monitorOverrides) {
          const std::vector<std::string>& monitorStart = ovr.startWidgets.has_value()
              ? editedItems({"bar", bar.name, "monitor", ovr.match, "start"}, *ovr.startWidgets)
              : baseStart;
          const std::vector<std::string>& monitorCenter = ovr.centerWidgets.has_value()
              ? editedItems({"bar", bar.name, "monitor", ovr.match, "center"}, *ovr.centerWidgets)
              : baseCenter;
          const std::vector<std::string>& monitorEnd = ovr.endWidgets.has_value()
              ? editedItems({"bar", bar.name, "monitor", ovr.match, "end"}, *ovr.endWidgets)
              : baseEnd;
          const auto& groups = ovr.widgetCapsuleGroups.has_value() ? *ovr.widgetCapsuleGroups : bar.widgetCapsuleGroups;
          if (scopeContains(monitorStart, monitorCenter, monitorEnd, groups, placements)
              || (ovr.sectionsSpecified && std::ranges::any_of(ovr.sections, [&](const BarSectionConfig& section) {
                   return std::ranges::any_of(section.widgets, [&](const std::string& widget) {
                     return noctalia::bar::resolveBarWidgetLaneEntry(widget, placements).widgetConfigName == widgetName;
                   });
                 }))) {
            return true;
          }
        }
      }
      return false;
    }

    bool isValidWidgetInstanceId(std::string_view id) {
      if (id.empty()) {
        return false;
      }
      for (char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
          return false;
        }
      }
      return true;
    }

    bool canRenameWidgetInstance(const Config& cfg, std::string_view oldName, std::string_view newName) {
      return isValidWidgetInstanceId(newName) && oldName != newName && !widgetReferenceNameExists(cfg, newName);
    }

    const BarConfig* barForLanePath(const Config& cfg, const std::vector<std::string>& path) {
      if (path.size() < 2 || path[0] != "bar") {
        return nullptr;
      }
      return findBar(cfg, path[1]);
    }

    const BarMonitorOverride* monitorOverrideForLanePath(const Config& cfg, const std::vector<std::string>& path) {
      if (!isMonitorWidgetListPath(path)) {
        return nullptr;
      }
      const BarConfig* bar = barForLanePath(cfg, path);
      return bar != nullptr ? findMonitorOverride(*bar, path[3]) : nullptr;
    }

    std::vector<std::string> capsuleGroupPathForLanePath(const std::vector<std::string>& lanePath) {
      if (isMonitorWidgetListPath(lanePath)) {
        return {"bar", lanePath[1], "monitor", lanePath[3], "capsule_group"};
      }
      if (lanePath.size() >= 2 && lanePath[0] == "bar") {
        return {"bar", lanePath[1], "capsule_group"};
      }
      return {};
    }

    std::vector<BarCapsuleGroupStyle>
    capsuleGroupsForLanePath(const Config& cfg, const std::vector<std::string>& lanePath) {
      const BarConfig* bar = barForLanePath(cfg, lanePath);
      if (bar == nullptr) {
        return {};
      }
      const BarMonitorOverride* ovr = monitorOverrideForLanePath(cfg, lanePath);
      if (ovr != nullptr && ovr->widgetCapsuleGroups.has_value()) {
        return *ovr->widgetCapsuleGroups;
      }
      return bar->widgetCapsuleGroups;
    }

    const BarCapsuleGroupStyle*
    findCapsuleGroupStyle(const std::vector<BarCapsuleGroupStyle>& groups, std::string_view id) {
      const auto it = std::ranges::find(groups, id, &BarCapsuleGroupStyle::id);
      return it != groups.end() ? &*it : nullptr;
    }

    // Smallest unused `g<N>` id within an owner scope's existing groups.
    std::string nextCapsuleGroupId(const std::vector<BarCapsuleGroupStyle>& groups) {
      int n = 1;
      std::string candidate = "g" + std::to_string(n);
      while (std::ranges::contains(groups, candidate, &BarCapsuleGroupStyle::id)) {
        candidate = "g" + std::to_string(++n);
      }
      return candidate;
    }

    // New group style seeded from the effective bar/monitor capsule defaults.
    BarCapsuleGroupStyle
    seedCapsuleGroupStyle(const Config& cfg, const std::vector<std::string>& lanePath, std::string id) {
      BarCapsuleGroupStyle group;
      group.id = std::move(id);
      const BarConfig* bar = barForLanePath(cfg, lanePath);
      if (bar == nullptr) {
        return group;
      }
      group.fill = bar->widgetCapsuleFill;
      group.borderSpecified = bar->widgetCapsuleBorderSpecified;
      group.border = bar->widgetCapsuleBorder;
      group.foreground = bar->widgetCapsuleForeground;
      group.padding = bar->widgetCapsulePadding;
      if (bar->widgetCapsuleRadius.has_value()) {
        group.radius = static_cast<float>(*bar->widgetCapsuleRadius);
      }
      group.opacity = bar->widgetCapsuleOpacity;

      const BarMonitorOverride* ovr = monitorOverrideForLanePath(cfg, lanePath);
      if (ovr == nullptr) {
        return group;
      }
      if (ovr->widgetCapsuleFill.has_value()) {
        group.fill = *ovr->widgetCapsuleFill;
      }
      if (ovr->widgetCapsuleBorderSpecified) {
        group.borderSpecified = true;
        group.border = ovr->widgetCapsuleBorder;
      }
      if (ovr->widgetCapsuleForeground.has_value()) {
        group.foreground = *ovr->widgetCapsuleForeground;
      }
      if (ovr->widgetCapsulePadding.has_value()) {
        group.padding = std::clamp(static_cast<float>(*ovr->widgetCapsulePadding), 0.0F, 48.0F);
      }
      if (ovr->widgetCapsuleRadius.has_value()) {
        group.radius = static_cast<float>(std::clamp(*ovr->widgetCapsuleRadius, 0.0, 80.0));
      }
      if (ovr->widgetCapsuleOpacity.has_value()) {
        group.opacity = std::clamp(static_cast<float>(*ovr->widgetCapsuleOpacity), 0.0F, 1.0F);
      }
      return group;
    }

    std::size_t insertionIndexForSceneY(float sceneY, const std::vector<Flex*>& itemNodes) {
      for (std::size_t i = 0; i < itemNodes.size(); ++i) {
        const auto* item = itemNodes[i];
        if (item == nullptr) {
          continue;
        }
        float ignoredX = 0.0F;
        float itemY = 0.0F;
        Node::absolutePosition(item, ignoredX, itemY);
        if (sceneY < itemY + item->height() * 0.5F) {
          return i;
        }
      }
      return itemNodes.size();
    }

    bool insertionWouldNotMove(
        std::size_t sourceZoneIndex, std::size_t targetZoneIndex, std::size_t fromIndex, std::size_t insertionIndex
    ) {
      return sourceZoneIndex == targetZoneIndex && (insertionIndex == fromIndex || insertionIndex == fromIndex + 1);
    }

    // Innermost zone containing the point. Group zones win over the lane that encloses them.
    std::optional<std::size_t> zoneAtScenePoint(const std::vector<DropZone>& zones, float sceneX, float sceneY) {
      std::optional<std::size_t> laneHit;
      for (std::size_t i = 0; i < zones.size(); ++i) {
        const auto* container = zones[i].container;
        if (container == nullptr) {
          continue;
        }
        float zoneX = 0.0F;
        float zoneY = 0.0F;
        Node::absolutePosition(container, zoneX, zoneY);
        const bool inside = sceneX >= zoneX
            && sceneX < zoneX + container->width()
            && sceneY >= zoneY
            && sceneY < zoneY + container->height();
        if (!inside) {
          continue;
        }
        if (zones[i].isGroup) {
          return i;
        }
        laneHit = i;
      }
      return laneHit;
    }

    void hideDropIndicators(const std::vector<DropZone>& zones) {
      for (const auto& zone : zones) {
        if (zone.indicator != nullptr) {
          zone.indicator->setVisible(false);
        }
      }
    }

    // Applies a drag move from (srcZone, srcIdx) to (dstZone, insertionIndex) by writing the affected lane
    // vectors and/or the bar's capsule-group vector. Group member edits funnel through one group-vector write.
    void performZoneMove(
        const Config& cfg, const std::vector<std::string>& laneListPath, const std::vector<DropZone>& zones,
        std::size_t srcZone, std::size_t srcIdx, std::size_t dstZone, std::size_t insertionIndex,
        const std::function<void(std::vector<std::string>, ConfigOverrideValue)>& setOverride,
        const std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)>& setOverrides
    ) {
      if (srcZone >= zones.size() || dstZone >= zones.size()) {
        return;
      }
      std::vector<std::string> srcItems = zones[srcZone].items;
      if (srcIdx >= srcItems.size()) {
        return;
      }
      const std::string moving = srcItems[srcIdx];
      const bool sameZone = srcZone == dstZone;
      // No nesting: a group token cannot be dropped inside a group.
      if (zones[dstZone].isGroup && isCapsuleGroupToken(moving)) {
        return;
      }
      if (sameZone && insertionWouldNotMove(srcZone, dstZone, srcIdx, insertionIndex)) {
        return;
      }

      srcItems.erase(srcItems.begin() + static_cast<std::ptrdiff_t>(srcIdx));
      std::vector<std::string> dstItems = sameZone ? srcItems : zones[dstZone].items;
      std::size_t insert = insertionIndex;
      if (sameZone && insertionIndex > srcIdx) {
        --insert;
      }
      insert = std::min(insert, dstItems.size());
      dstItems.insert(dstItems.begin() + static_cast<std::ptrdiff_t>(insert), moving);

      std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(cfg, laneListPath);
      bool groupsTouched = false;
      // Lane edits keyed by zone index, so a later empty-group cleanup can also drop a token from a lane.
      std::vector<std::pair<std::size_t, std::vector<std::string>>> laneEdits;
      const auto setLane = [&](std::size_t zoneIndex, std::vector<std::string> items) {
        for (auto& edit : laneEdits) {
          if (edit.first == zoneIndex) {
            edit.second = std::move(items);
            return;
          }
        }
        laneEdits.emplace_back(zoneIndex, std::move(items));
      };
      const auto laneItemsFor = [&](std::size_t zoneIndex) {
        for (const auto& edit : laneEdits) {
          if (edit.first == zoneIndex) {
            return edit.second;
          }
        }
        return zones[zoneIndex].items;
      };
      const auto applyZone = [&](std::size_t zoneIndex, const std::vector<std::string>& items) {
        const DropZone& zone = zones[zoneIndex];
        if (zone.isGroup) {
          for (auto& g : groups) {
            if (g.id == zone.groupId) {
              g.members = items;
              break;
            }
          }
          groupsTouched = true;
        } else {
          setLane(zoneIndex, items);
        }
      };
      if (sameZone) {
        applyZone(srcZone, dstItems);
      } else {
        applyZone(srcZone, srcItems);
        applyZone(dstZone, dstItems);
      }

      // Dragging the last member out empties a group: drop it and its lane token.
      if (groupsTouched) {
        for (const auto& g : groups) {
          if (!g.members.empty()) {
            continue;
          }
          const std::string token = makeCapsuleGroupToken(g.id);
          for (std::size_t zi = 0; zi < zones.size(); ++zi) {
            if (zones[zi].isGroup) {
              continue;
            }
            std::vector<std::string> items = laneItemsFor(zi);
            const auto it = std::ranges::find(items, token);
            if (it != items.end()) {
              items.erase(it);
              setLane(zi, std::move(items));
            }
          }
        }
        std::vector<BarCapsuleGroupStyle> kept;
        for (auto& g : groups) {
          if (!g.members.empty()) {
            kept.push_back(std::move(g));
          }
        }
        groups.swap(kept);
      }

      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> batch;
      batch.reserve(laneEdits.size());
      for (const auto& edit : laneEdits) {
        batch.emplace_back(zones[edit.first].lanePath, edit.second);
      }
      if (groupsTouched) {
        const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(laneListPath);
        if (!groupPath.empty()) {
          batch.emplace_back(groupPath, groups);
        }
      }
      if (batch.size() == 1) {
        setOverride(batch[0].first, batch[0].second);
      } else if (!batch.empty()) {
        setOverrides(batch);
      }
    }

    // Index of the item under sceneY, plus whether the pointer is in its middle band (a "combine" gesture
    // rather than an insertion between items).
    std::optional<std::pair<std::size_t, bool>> hoveredItemBand(float sceneY, const std::vector<Flex*>& itemNodes) {
      for (std::size_t i = 0; i < itemNodes.size(); ++i) {
        const auto* node = itemNodes[i];
        if (node == nullptr) {
          continue;
        }
        float nodeX = 0.0F;
        float nodeY = 0.0F;
        Node::absolutePosition(node, nodeX, nodeY);
        const float h = node->height();
        if (h > 0.0F && sceneY >= nodeY && sceneY < nodeY + h) {
          const float rel = (sceneY - nodeY) / h;
          return std::make_pair(i, rel > 0.3F && rel < 0.7F);
        }
      }
      return std::nullopt;
    }

    // Toggles the combine-target outline on a loose widget card (reset matches makeWidgetCard's default border).
    void
    setCardCombineHighlight(const std::vector<DropZone>& zones, std::size_t zoneIndex, std::size_t itemIndex, bool on) {
      if (zoneIndex >= zones.size()
          || zones[zoneIndex].itemNodes == nullptr
          || itemIndex >= zones[zoneIndex].itemNodes->size()) {
        return;
      }
      Flex* card = (*zones[zoneIndex].itemNodes)[itemIndex];
      if (card == nullptr) {
        return;
      }
      if (on) {
        card->setBorder(colorSpecFromRole(ColorRole::Primary), Style::borderWidth * 2.0F);
      } else {
        card->setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
      }
    }

    // Creates a new group from two loose widgets (the dragged one dropped onto the target). The target keeps
    // its lane position (now a group token); the dragged widget is pulled from its source lane.
    void createGroupByCombine(
        const Config& cfg, const std::vector<DropZone>& zones, std::size_t draggedZone, std::size_t draggedIdx,
        std::size_t targetZone, std::size_t targetIdx,
        const std::function<void(std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>)>& setOverrides
    ) {
      if (draggedZone >= zones.size()
          || targetZone >= zones.size()
          || zones[draggedZone].isGroup
          || zones[targetZone].isGroup) {
        return;
      }
      const DropZone& dz = zones[draggedZone];
      const DropZone& tz = zones[targetZone];
      if (draggedIdx >= dz.items.size() || targetIdx >= tz.items.size()) {
        return;
      }
      const std::string draggedName = dz.items[draggedIdx];
      const std::string targetName = tz.items[targetIdx];
      if (isCapsuleGroupToken(draggedName) || isCapsuleGroupToken(targetName)) {
        return;
      }
      const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(dz.lanePath);
      if (groupPath.empty()) {
        return;
      }
      std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(cfg, dz.lanePath);
      const std::string newId = nextCapsuleGroupId(groups);
      BarCapsuleGroupStyle newGroup = seedCapsuleGroupStyle(cfg, dz.lanePath, newId);
      newGroup.members = {targetName, draggedName};
      groups.push_back(std::move(newGroup));
      const std::string token = makeCapsuleGroupToken(newId);

      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> batch;
      if (draggedZone == targetZone) {
        std::vector<std::string> lane;
        lane.reserve(dz.items.size());
        for (std::size_t k = 0; k < dz.items.size(); ++k) {
          if (k == draggedIdx) {
            continue;
          }
          if (k == targetIdx) {
            lane.push_back(token);
            continue;
          }
          lane.push_back(dz.items[k]);
        }
        batch.emplace_back(dz.lanePath, lane);
      } else {
        std::vector<std::string> draggedLane = dz.items;
        draggedLane.erase(draggedLane.begin() + static_cast<std::ptrdiff_t>(draggedIdx));
        std::vector<std::string> targetLane = tz.items;
        targetLane[targetIdx] = token;
        batch.emplace_back(dz.lanePath, draggedLane);
        batch.emplace_back(tz.lanePath, targetLane);
      }
      batch.emplace_back(groupPath, groups);
      setOverrides(batch);
    }

    void updateDropIndicator(
        Box& indicator, const Flex& lane, const std::vector<Flex*>& itemNodes, std::size_t insertionIndex, float scale
    ) {
      if (insertionIndex > itemNodes.size()) {
        indicator.setVisible(false);
        return;
      }

      const float x = Style::spaceSm * scale;
      const float width = std::max(1.0F, lane.width() - Style::spaceSm * scale * 2.0F);
      const float gapHalf = Style::spaceXs * scale * 0.5F;
      float y = Style::controlHeightSm * scale + Style::spaceSm * scale;
      if (!itemNodes.empty()) {
        if (insertionIndex == itemNodes.size()) {
          const auto* target = itemNodes.back();
          y = target != nullptr ? target->y() + target->height() + gapHalf : y;
        } else {
          const auto* target = itemNodes[insertionIndex];
          y = target != nullptr ? target->y() - gapHalf : y;
        }
      }

      indicator.setPosition(x, y);
      indicator.setFrameSize(width, std::max(2.0F, 3.0F * scale));
      indicator.setVisible(true);
    }

    std::vector<std::string> widgetSettingPath(std::string widgetName, std::string settingKey) {
      return {"widget", std::move(widgetName), std::move(settingKey)};
    }

    WidgetSettingValue
    widgetSettingValue(const Config& cfg, std::string_view widgetName, const WidgetSettingSpec& spec) {
      if (const auto it = cfg.widgets.find(std::string(widgetName)); it != cfg.widgets.end()) {
        if (const auto settingIt = it->second.settings.find(spec.schema.key); settingIt != it->second.settings.end()) {
          return settingIt->second;
        }
      }
      return spec.schema.defaultValue;
    }

    std::string reservedGestureDescription(std::string_view type, noctalia::bar::Gesture gesture) {
      using noctalia::bar::Gesture;
      if (type == "workspaces" && gesture == Gesture::Left) return "Activate the selected workspace";
      if (type == "taskbar" && gesture == Gesture::Left) return "Activate the selected application";
      if (type == "taskbar" && gesture == Gesture::Middle) return "Close the selected application";
      if (type == "tray" && gesture == Gesture::Left) return "Activate the selected tray item";
      if (type == "tray" && gesture == Gesture::Right) return "Open the selected tray item's menu";
      if (type == "screenshot" && gesture == Gesture::Right) return "Open the capture menu";
      return {};
    }

    std::string widgetActionConfigValue(const noctalia::bar::WidgetAction& action) {
      return action.kind == noctalia::bar::WidgetAction::Kind::Exec
          ? "exec " + action.args : action.commandLine();
    }

    std::string describeWidgetAction(const noctalia::bar::WidgetAction* action) {
      if (action == nullptr) return "None";
      if (action->kind == noctalia::bar::WidgetAction::Kind::Exec) return "Run command: " + action->args;
      return action->commandLine();
    }

    WidgetSettingStringMap inheritedWidgetActionDefaults(
        std::string_view widgetName, std::string_view widgetType, const WidgetConfig* widgetConfig,
        const BarConfig* bar
    ) {
      const auto typeDefaults = noctalia::bar::gestureDefaultsForType(widgetType, widgetConfig);
      noctalia::bar::WidgetActionBindings bindings;
      bindings.resolve({
          .builtinDefaults = noctalia::bar::builtinGestureDefaults(),
          .widgetDefaults = typeDefaults,
          .barActions = bar != nullptr ? &bar->actions : nullptr,
          .reserved = noctalia::bar::reservedGesturesForType(widgetType),
          .widgetContext = std::format("widget.{}", widgetName),
          .barContext = bar != nullptr ? std::format("bar.{}", bar->name) : "bar",
          .widgetName = widgetName,
          .widgetType = widgetType,
      });

      WidgetSettingStringMap defaults;
      for (const auto gesture : noctalia::bar::allGestures()) {
        if (const auto* action = bindings.find(gesture); action != nullptr) {
          defaults.insert_or_assign(
              std::string(noctalia::bar::gestureConfigKey(gesture)), widgetActionConfigValue(*action)
          );
        }
      }
      return defaults;
    }

    std::unique_ptr<Node> makeWidgetBehaviorSummary(
        const Config& cfg, std::string_view widgetName, std::string_view widgetType,
        const WidgetConfig* widgetConfig, const std::vector<std::string>& lanePath,
        const std::vector<WidgetSettingSpec>& specs, float scale
    ) {
      const BarConfig* bar = lanePath.size() >= 2 && lanePath[0] == "bar" ? findBar(cfg, lanePath[1]) : nullptr;
      const auto typeDefaults = noctalia::bar::gestureDefaultsForType(widgetType, widgetConfig);
      noctalia::bar::WidgetActionBindings bindings;
      bindings.resolve({
          .builtinDefaults = noctalia::bar::builtinGestureDefaults(),
          .widgetDefaults = typeDefaults,
          .barActions = bar != nullptr ? &bar->actions : nullptr,
          .widgetActions = noctalia::bar::findActionTable(widgetConfig),
          .reserved = noctalia::bar::reservedGesturesForType(widgetType),
          .widgetContext = std::format("widget.{}", widgetName),
          .barContext = bar != nullptr ? std::format("bar.{}", bar->name) : "bar",
          .widgetName = widgetName,
          .widgetType = widgetType,
      });

      bool interactive = widgetType != "spacer";
      if (const auto spec = std::ranges::find(specs, "interactive", [](const WidgetSettingSpec& candidate) {
            return std::string_view(candidate.schema.key);
          }); spec != specs.end()) {
        if (const auto value = widgetSettingValue(cfg, widgetName, *spec); const auto* enabled = std::get_if<bool>(&value)) {
          interactive = *enabled;
        }
      } else if (widgetConfig != nullptr) {
        interactive = widgetConfig->getBool("interactive", interactive);
      }

      bool revealsOnHover = false;
      if (const auto spec = std::ranges::find(specs, "title_scroll", [](const WidgetSettingSpec& candidate) {
            return std::string_view(candidate.schema.key);
          }); spec != specs.end()) {
        const auto titleScroll = widgetSettingValue(cfg, widgetName, *spec);
        revealsOnHover = std::get_if<std::string>(&titleScroll) != nullptr
            && *std::get_if<std::string>(&titleScroll) == "on_hover";
      }
      std::string hover;
      if (!interactive) hover = "Disabled (pointer input passes through)";
      else if (widgetType == "workspaces") hover = "Highlight workspace item";
      else if (widgetType == "taskbar") hover = "Highlight task item";
      else if (widgetType == "tray") hover = "Highlight tray item";
      else hover = bar != nullptr && bar->hoverHighlight ? "Highlight module" : "None";
      if (interactive && revealsOnHover) hover = hover == "None" ? "Scroll title" : hover + "; scroll title";

      auto body = ui::column({.align = FlexAlign::Stretch, .gap = Style::spaceXs * scale});
      body->addChild(makeLabel(
          "Action inheritance: built-in, module type, bar-wide, then module instance. Pointer fallback: child control or module first; uncovered section background (or legacy lane) next; bar dead_zone last.",
          Style::fontSizeMini * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      body->addChild(makeLabel(
          "Hover · " + hover, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurface)
      ));
      const auto reserved = noctalia::bar::reservedGesturesForType(widgetType);
      for (const auto gesture : noctalia::bar::allGestures()) {
        std::string action = interactive ? std::string{} : "Disabled (handled by section or bar)";
        if (interactive && reserved.contains(gesture)) action = reservedGestureDescription(widgetType, gesture);
        if (interactive && action.empty()) action = describeWidgetAction(bindings.find(gesture));
        body->addChild(makeLabel(
            i18n::tr(std::string(noctalia::bar::gestureLabelKey(gesture))) + " · " + action,
            Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurface)
        ));
      }
      const std::string scrollMode = widgetConfig != nullptr
          ? widgetConfig->getString("scroll_repeat", "auto")
          : "auto";
      body->addChild(makeLabel(
          "Scroll delivery · " + (!interactive ? std::string("Disabled (handled by section or bar)")
              : scrollMode == "steps" ? std::string("Every wheel detent")
              : scrollMode == "gesture" ? std::string("Once per continuous gesture")
                                        : std::string("Cycle actions once; other actions every detent")),
          Style::fontSizeMini * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));
      return body;
    }

    [[nodiscard]] bool isBarHorizontal(const Config& cfg, std::string_view barName) {
      const BarConfig* bar = findBar(cfg, barName);
      if (bar == nullptr) {
        return true;
      }
      return bar->position != "left" && bar->position != "right";
    }

    bool settingValueAsBool(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<bool>(&value)) {
        return *v;
      }
      return false;
    }

    std::int64_t settingValueAsInt(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::int64_t>(&value)) {
        return *v;
      }
      if (const auto* v = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(std::llround(*v));
      }
      return std::int64_t{0};
    }

    double settingValueAsDouble(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<double>(&value)) {
        return *v;
      }
      if (const auto* v = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*v);
      }
      return 0.0;
    }

    std::optional<double>
    widgetSettingOptionalDouble(const Config& cfg, std::string_view widgetName, const std::string& key) {
      if (const auto it = cfg.widgets.find(std::string(widgetName)); it != cfg.widgets.end()) {
        if (const auto settingIt = it->second.settings.find(key); settingIt != it->second.settings.end()) {
          if (const auto* v = std::get_if<double>(&settingIt->second)) {
            return *v;
          }
          if (const auto* v = std::get_if<std::int64_t>(&settingIt->second)) {
            return static_cast<double>(*v);
          }
        }
      }
      return std::nullopt;
    }

    std::optional<int>
    widgetSettingOptionalStepperValue(const Config& cfg, std::string_view widgetName, const std::string& key) {
      const auto value = widgetSettingOptionalDouble(cfg, widgetName, key);
      if (!value.has_value()) {
        return std::nullopt;
      }
      return std::clamp(static_cast<int>(std::lround(*value)), 0, 80);
    }

    int inheritedCapsuleRadiusForLane(const Config& cfg, const std::vector<std::string>& lanePath) {
      if (lanePath.size() < 2 || lanePath[0] != "bar") {
        return 8;
      }
      const BarConfig* bar = findBar(cfg, lanePath[1]);
      if (bar == nullptr) {
        return 8;
      }
      if (isMonitorWidgetListPath(lanePath) && lanePath.size() >= 4) {
        if (const auto* ovr = findMonitorOverride(*bar, lanePath[3]);
            ovr != nullptr && ovr->widgetCapsuleRadius.has_value()) {
          return std::clamp(static_cast<int>(std::lround(*ovr->widgetCapsuleRadius)), 0, 80);
        }
      }
      if (bar->widgetCapsuleRadius.has_value()) {
        return std::clamp(static_cast<int>(std::lround(*bar->widgetCapsuleRadius)), 0, 80);
      }
      return 8;
    }

    std::string settingValueAsString(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::string>(&value)) {
        return *v;
      }
      if (const auto* v = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*v);
      }
      return {};
    }

    std::string widgetLabelFontWeightSelectedValue(const Config& cfg, std::string_view widgetName) {
      const auto widgetIt = cfg.widgets.find(std::string(widgetName));
      if (widgetIt == cfg.widgets.end()) {
        return {};
      }
      const auto settingIt = widgetIt->second.settings.find("font_weight");
      if (settingIt == widgetIt->second.settings.end()) {
        return {};
      }
      return settingValueAsString(settingIt->second);
    }

    // Effective typeface for the widget's labels: its own font_family override, else the hosting bar's
    // font_family, else the global shell font. Used to list only the weights the real font provides.
    std::string widgetResolvedFontFamily(const Config& cfg, std::string_view widgetName) {
      if (const auto widgetIt = cfg.widgets.find(std::string(widgetName)); widgetIt != cfg.widgets.end()) {
        const auto it = widgetIt->second.settings.find("font_family");
        if (it != widgetIt->second.settings.end()) {
          const std::string family = settingValueAsString(it->second);
          if (!family.empty()) {
            return family;
          }
        }
      }
      const auto inLane = [&](const std::vector<std::string>& lane) { return std::ranges::contains(lane, widgetName); };
      for (const BarConfig& bar : cfg.bars) {
        if (inLane(bar.startWidgets) || inLane(bar.centerWidgets) || inLane(bar.endWidgets)) {
          if (bar.fontFamily && !bar.fontFamily->empty()) {
            return *bar.fontFamily;
          }
          break;
        }
      }
      return cfg.shell.fontFamily;
    }

    std::vector<std::string> settingValueAsStringList(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::vector<std::string>>(&value)) {
        return *v;
      }
      return {};
    }

    std::string settingValueAsDisplayString(const WidgetSettingValue& value) {
      return std::visit(
          [](const auto& concrete) -> std::string {
            using T = std::decay_t<decltype(concrete)>;
            if constexpr (std::is_same_v<T, bool>) {
              return concrete ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
              return std::to_string(concrete);
            } else if constexpr (std::is_same_v<T, double>) {
              return std::format("{}", concrete);
            } else if constexpr (std::is_same_v<T, std::string>) {
              return "\"" + concrete + "\"";
            } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
              std::string out = "[";
              for (std::size_t i = 0; i < concrete.size(); ++i) {
                if (i > 0) {
                  out += ", ";
                }
                out += "\"" + concrete[i] + "\"";
              }
              out += "]";
              return out;
            } else if constexpr (std::is_same_v<T, WidgetSettingStringMap>) {
              std::vector<std::string> keys;
              keys.reserve(concrete.size());
              for (const auto& [key, mapValue] : concrete) {
                (void)mapValue;
                keys.push_back(key);
              }
              std::ranges::sort(keys);
              std::string out = "{";
              for (std::size_t i = 0; i < keys.size(); ++i) {
                if (i > 0) {
                  out += ", ";
                }
                out += "\"" + keys[i] + "\" = \"" + concrete.at(keys[i]) + "\"";
              }
              out += "}";
              return out;
            }
          },
          value
      );
    }

    SelectSetting labelFontWeightSelectSetting(
        const WidgetSettingSpec& spec, std::string selectedValue, std::string_view fontFamily
    ) {
      std::optional<int> preserveWeight;
      if (!selectedValue.empty()) {
        preserveWeight = static_cast<int>(std::strtol(selectedValue.c_str(), nullptr, 10));
      }

      std::vector<SelectOption> options;
      const auto catalogOptions =
          buildLabelFontWeightSelectOptions(fontFamily, FontWeightSelectKind::WidgetInheritDefault, preserveWeight);
      options.reserve(catalogOptions.size());
      for (const auto& option : catalogOptions) {
        options.push_back(SelectOption{option.value, i18n::tr(option.labelKey)});
      }

      SelectSetting selectSetting{std::move(options), std::move(selectedValue)};
      selectSetting.valueType = spec.integerValue ? SelectValueType::Integer : SelectValueType::String;
      return selectSetting;
    }

    SelectSetting
    sourcedSelectSetting(const BarWidgetEditorContext& ctx, const WidgetSettingSpec& spec, std::string selectedValue) {
      std::vector<SelectOption> options;
      const auto appendUnique = [&](SelectOption option) {
        if (!std::ranges::contains(options, option.value, &SelectOption::value)) {
          options.push_back(std::move(option));
        }
      };
      options.reserve(spec.options.size());
      for (const auto& option : spec.options) {
        appendUnique(
            SelectOption{
                .value = option.value,
                .label = spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey),
            }
        );
      }

      std::vector<SelectOption> sourcedOptions;
      switch (spec.optionSource) {
      case WidgetSettingOptionSource::BatteryDevices:
        sourcedOptions = ctx.batteryDeviceOptions;
        break;
      case WidgetSettingOptionSource::Static:
        break;
      }
      options.reserve(options.size() + sourcedOptions.size());
      for (auto& option : sourcedOptions) {
        appendUnique(std::move(option));
      }

      const auto hasEmptyOption = std::ranges::contains(options, std::string_view{}, &SelectOption::value);
      if (selectedValue.empty() && !hasEmptyOption) {
        selectedValue = settingValueAsString(spec.schema.defaultValue);
      }

      const auto hasSelected = std::ranges::contains(options, selectedValue, &SelectOption::value);
      if (!selectedValue.empty() && !hasSelected) {
        options.push_back(
            SelectOption{
                .value = selectedValue,
                .label = i18n::tr("settings.controls.select.unknown-value", "value", selectedValue),
            }
        );
      }

      return SelectSetting{std::move(options), std::move(selectedValue)};
    }

    void addRawWidgetSettings(
        Flex& panel, std::string_view widgetName, const std::vector<WidgetSettingSpec>& specs,
        std::size_t& visibleSpecs, const BarWidgetEditorContext& ctx
    ) {
      if (!ctx.showAdvanced) {
        return;
      }

      const auto widgetIt = ctx.config.widgets.find(std::string(widgetName));
      if (widgetIt == ctx.config.widgets.end()) {
        return;
      }

      std::unordered_set<std::string> knownKeys;
      knownKeys.reserve(specs.size());
      for (const auto& spec : specs) {
        knownKeys.insert(spec.schema.key);
      }

      std::vector<std::string> rawKeys;
      for (const auto& [key, value] : widgetIt->second.settings) {
        if (knownKeys.contains(key)) {
          continue;
        }
        const auto path = widgetSettingPath(std::string(widgetName), key);
        const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(path);
        if (ctx.showOverriddenOnly && !overridden) {
          continue;
        }
        rawKeys.push_back(key);
      }

      if (rawKeys.empty()) {
        return;
      }
      std::ranges::sort(rawKeys);

      panel.addChild(
          ui::column(
              {
                  .align = FlexAlign::Stretch,
                  .gap = 1.0F * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = 0,
              },
              makeLabel(
                  i18n::tr("settings.entities.widget.raw.title"), Style::fontSizeCaption * ctx.scale,
                  colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
              ),
              makeSettingSubtitleLabel(i18n::tr("settings.entities.widget.raw.description"), ctx.scale)
          )
      );

      for (const auto& key : rawKeys) {
        const auto valueIt = widgetIt->second.settings.find(key);
        if (valueIt == widgetIt->second.settings.end()) {
          continue;
        }
        const auto path = widgetSettingPath(std::string(widgetName), key);
        const std::string deleteKey = pathKey(path);
        const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(path);
        const bool pendingDelete = ctx.pendingDeleteWidgetSettingPath == deleteKey;

        auto row = ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = 0,
                .minHeight = Style::controlHeightSm * ctx.scale,
            },
            makeLabel(
                key, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
            ),
            ui::spacer(),
            makeLabel(
                settingValueAsDisplayString(valueIt->second), Style::fontSizeCaption * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
            )
        );

        if (overridden) {
          row->addChild(
              ui::button({
                  .text = pendingDelete ? std::optional<std::string>(i18n::tr("settings.entities.widget.raw.delete"))
                                        : std::nullopt,
                  .glyph = "trash",
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .glyphSize = Style::fontSizeCaption * ctx.scale,
                  .variant = pendingDelete ? ButtonVariant::Default : ButtonVariant::Ghost,
                  .minWidth = Style::controlHeightSm * ctx.scale,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .padding = Style::spaceXs * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [&pendingDeleteWidgetSettingPath = ctx.pendingDeleteWidgetSettingPath, deleteKey, path,
                              clearOverride = ctx.clearOverride, requestRebuild = ctx.requestRebuild]() {
                    if (pendingDeleteWidgetSettingPath != deleteKey) {
                      pendingDeleteWidgetSettingPath = deleteKey;
                      requestRebuild();
                      return;
                    }

                    pendingDeleteWidgetSettingPath.clear();
                    clearOverride(path);
                  },
              })
          );
        }

        panel.addChild(std::move(row));
        ++visibleSpecs;
      }
    }

    void addWidgetSettingsPanel(
        Flex& item, std::string widgetName, const std::vector<std::string>& lanePath, const BarWidgetEditorContext& ctx
    ) {
      const auto widgetType = widgetTypeForReference(ctx.config, widgetName);
      if (widgetType.empty()) {
        return;
      }

      const auto widgetIt = ctx.config.widgets.find(widgetName);
      const WidgetConfig* widgetConfig = widgetIt != ctx.config.widgets.end() ? &widgetIt->second : nullptr;
      auto specs = widgetSettingSpecs(widgetType, widgetConfig, ctx.config.shell.fontFamily);
      if (specs.empty()) {
        return;
      }

      auto panel = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * ctx.scale,
      });

      panel->addChild(makeMiniSectionHeader("Behavior", ctx.scale, false));
      panel->addChild(makeWidgetBehaviorSummary(
          ctx.config, widgetName, widgetType, widgetConfig, lanePath, specs, ctx.scale
      ));

      std::size_t visibleSpecs = 0;
      std::string activeGroupKey;
      // Coalesce specs by group so each group header renders once regardless of spec declaration order.
      const auto specOrder = coalesceByGroupKey(specs.size(), [&](std::size_t i) { return specs[i].group; });
      const bool barHorizontal =
          lanePath.size() >= 2 && lanePath[0] == "bar" ? isBarHorizontal(ctx.config, lanePath[1]) : true;
      for (const std::size_t specIndex : specOrder) {
        const auto& spec = specs[specIndex];
        if (!spec.visibleInInspector) {
          continue;
        }
        if (spec.horizontalBarOnly && !barHorizontal) {
          continue;
        }
        if (!widgetSettingIsVisible(
                ctx.config, widgetName, spec, specs,
                WidgetSettingCapabilities{
                    .taskbarWorkspaceGrouping = ctx.supportsTaskbarWorkspaceGrouping,
                }
            )) {
          continue;
        }
        if (spec.advanced && !ctx.showAdvanced) {
          continue;
        }
        const auto path = widgetSettingPath(widgetName, spec.schema.key);
        const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveOverride(path);
        if (ctx.showOverriddenOnly && !overridden) {
          continue;
        }

        if (spec.group != activeGroupKey) {
          // The actions group folds, and carries its title in the collapsible's own header.
          if (spec.group != kGestureActionsGroup) {
            panel->addChild(makeMiniSectionHeader(widgetSettingGroupTitle(spec.group), ctx.scale, visibleSpecs > 0));
          }
          activeGroupKey = spec.group;
        }

        const auto value = widgetSettingValue(ctx.config, widgetName, spec);
        SettingEntry entry{
            .section = SettingsSection::Bar,
            .group = "widget-settings",
            .title = !spec.literalLabel.empty() ? spec.literalLabel
                : spec.labelKey.empty()         ? std::string{}
                                                : i18n::tr(spec.labelKey),
            .subtitle = !spec.literalDescription.empty() ? spec.literalDescription
                : spec.descriptionKey.empty()            ? std::string{}
                                                         : i18n::tr(spec.descriptionKey),
            .path = path,
            .control = TextSetting{},
            .advanced = spec.advanced,
            .searchText = {},
        };

        const auto makeGlyphTextControl = [&ctx, path](std::string currentValue) -> std::unique_ptr<Node> {
          auto textNode = ctx.makeText(currentValue, {}, path);
          return ui::row(
              {
                  .align = FlexAlign::Center,
                  .gap = Style::spaceSm * ctx.scale,
              },
              std::move(textNode),
              ui::button({
                  .glyph = "apps",
                  .glyphSize = Style::fontSizeBody * ctx.scale,
                  .variant = ButtonVariant::Default,
                  .minWidth = Style::controlHeight * ctx.scale,
                  .minHeight = Style::controlHeight * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = Style::spaceSm * ctx.scale,
                  .radius = Style::scaledRadiusMd(ctx.scale),
                  .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, path,
                              currentValue = std::move(currentValue)]() {
                    GlyphPickerDialogOptions options;
                    if (!currentValue.empty()) {
                      options.initialGlyph = currentValue;
                    }
                    (void)GlyphPickerDialog::open(
                        std::move(options),
                        [setOverride, requestRebuild, path](std::optional<GlyphPickerResult> result) {
                          if (!result.has_value()) {
                            return;
                          }
                          setOverride(path, result->name);
                          if (requestRebuild) {
                            requestRebuild();
                          }
                        }
                    );
                  },
              })
          );
        };

        switch (spec.control) {
        case WidgetControlKind::Bool: {
          std::optional<bool> clearWhenValue;
          if (const auto* defaultBool = std::get_if<bool>(&spec.schema.defaultValue)) {
            clearWhenValue = *defaultBool;
          }
          ctx.makeRow(*panel, entry, ctx.makeToggle(settingValueAsBool(value), path, clearWhenValue));
          break;
        }
        case WidgetControlKind::Int: {
          // A plugin manifest may declare minValue > maxValue; order the range so
          // the clamp, stepper, and slider below all get a valid [min, max].
          const double rawMin = spec.schema.minValue.value_or(0.0);
          const double rawMax = spec.schema.maxValue.value_or(100.0);
          const double minValue = std::min(rawMin, rawMax);
          const double maxValue = std::max(rawMin, rawMax);
          if (spec.stepper) {
            const int minStep = static_cast<int>(std::lround(minValue));
            const int maxStep = static_cast<int>(std::lround(maxValue));
            const int stepValue = static_cast<int>(std::clamp(
                settingValueAsInt(value), static_cast<std::int64_t>(minStep), static_cast<std::int64_t>(maxStep)
            ));
            ctx.makeRow(
                *panel, entry,
                ctx.makeStepper(
                    StepperSetting{
                        .value = stepValue,
                        .minValue = minStep,
                        .maxValue = maxStep,
                        .step = static_cast<int>(std::max(1.0, spec.schema.step.value_or(1.0))),
                        .valueSuffix = spec.valueSuffix,
                    },
                    path
                )
            );
          } else {
            ctx.makeRow(
                *panel, entry,
                ctx.makeSlider(
                    static_cast<double>(settingValueAsInt(value)), minValue, maxValue, spec.schema.step.value_or(1.0),
                    path, true
                )
            );
          }
          break;
        }
        case WidgetControlKind::Double: {
          const double minValue = spec.schema.minValue.value_or(0.0);
          const double maxValue = spec.schema.maxValue.value_or(1.0);
          ctx.makeRow(
              *panel, entry,
              ctx.makeSlider(
                  settingValueAsDouble(value), minValue, maxValue, spec.schema.step.value_or(1.0), path, false
              )
          );
          break;
        }
        case WidgetControlKind::OptionalDouble: {
          ctx.makeRow(
              *panel, entry,
              ctx.makeOptionalStepper(
                  OptionalStepperSetting{
                      .value = widgetSettingOptionalStepperValue(ctx.config, widgetName, spec.schema.key),
                      .minValue = static_cast<int>(std::lround(spec.schema.minValue.value_or(0.0))),
                      .maxValue = static_cast<int>(std::lround(spec.schema.maxValue.value_or(80.0))),
                      .step = static_cast<int>(std::max(1.0, spec.schema.step.value_or(1.0))),
                      .fallbackValue = inheritedCapsuleRadiusForLane(ctx.config, lanePath),
                      .unsetLabel = i18n::tr("common.states.inherit"),
                      .customLabel = i18n::tr("common.states.custom")
                  },
                  path
              )
          );
          break;
        }
        case WidgetControlKind::String: {
          if (spec.schema.key == "custom_image") {
            FileDialogOptions options;
            options.mode = FileDialogMode::Open;
            options.defaultViewMode = FileDialogViewMode::Grid;
            options.title = i18n::tr("settings.widgets.settings.custom-image.dialog-title");
            options.extensions = DirectoryScanner::imageExtensionFilter(true);
            options.startDirectory = "/usr/share/icons";
            ctx.makeRow(
                *panel, entry,
                makePathBrowseControl(
                    ctx, path, settingValueAsString(value), "photo", std::move(options), PathBrowseKind::File
                )
            );
          } else {
            ctx.makeRow(*panel, entry, ctx.makeText(settingValueAsString(value), {}, path));
          }
          break;
        }
        case WidgetControlKind::File: {
          FileDialogOptions options;
          options.mode = FileDialogMode::Open;
          options.defaultViewMode = FileDialogViewMode::List;
          options.title = i18n::tr("settings.controls.path-browse.file-title");
          options.extensions = spec.extensions;
          ctx.makeRow(
              *panel, entry,
              makePathBrowseControl(
                  ctx, path, settingValueAsString(value), "file-text", std::move(options), PathBrowseKind::File
              )
          );
          break;
        }
        case WidgetControlKind::Folder: {
          FileDialogOptions options;
          options.mode = FileDialogMode::SelectFolder;
          options.defaultViewMode = FileDialogViewMode::List;
          options.title = i18n::tr("settings.controls.path-browse.folder-title");
          ctx.makeRow(
              *panel, entry,
              makePathBrowseControl(
                  ctx, path, settingValueAsString(value), "folder", std::move(options), PathBrowseKind::Folder
              )
          );
          break;
        }
        case WidgetControlKind::Glyph:
          ctx.makeRow(*panel, entry, makeGlyphTextControl(settingValueAsString(value)));
          break;
        case WidgetControlKind::StringList:
          ctx.makeListBlock(*panel, entry, ListSetting{.items = settingValueAsStringList(value)});
          break;
        case WidgetControlKind::StringMap: {
          // Gesture bindings have a closed key set, so they get one fixed row per gesture rather
          // than the free-form key/value editor.
          if (spec.schema.key == "actions") {
            const BarConfig* hostingBar = lanePath.size() >= 2 && lanePath[0] == "bar"
                ? findBar(ctx.config, lanePath[1]) : nullptr;
            WidgetSettingStringMap defaults = inheritedWidgetActionDefaults(
                widgetName, widgetType, widgetConfig, hostingBar
            );
            WidgetSettingStringMap configured;
            if (widgetConfig != nullptr) {
              if (const auto tableIt = widgetConfig->tables.find(spec.schema.key);
                  tableIt != widgetConfig->tables.end()) {
                configured = tableIt->second;
              }
            }
            auto body = ui::column({.align = FlexAlign::Stretch});
            addGestureActionRows(
                *body, ctx, entry, defaults, configured, noctalia::bar::reservedGesturesForType(widgetType)
            );
            panel->addChild(makeGestureActionsSection(ctx, widgetName, std::move(body), visibleSpecs > 0));
            break;
          }
          const bool effectsProfileGlyphs = spec.schema.key == "effects_profile_glyphs";
          WidgetSettingStringMap entries;
          if (widgetConfig != nullptr) {
            if (const auto tableIt = widgetConfig->tables.find(spec.schema.key);
                tableIt != widgetConfig->tables.end()) {
              entries = tableIt->second;
            } else if (const auto* defaults = std::get_if<WidgetSettingStringMap>(&spec.schema.defaultValue)) {
              entries = *defaults;
            }
          } else if (const auto* defaults = std::get_if<WidgetSettingStringMap>(&spec.schema.defaultValue)) {
            entries = *defaults;
          }
          ctx.makeStringMapBlock(
              *panel, entry,
              StringMapSetting{
                  .entries = std::move(entries),
                  .suggestedKeys = {},
                  .keyPlaceholder = i18n::tr(
                      effectsProfileGlyphs ? "settings.widgets.map-placeholders.effects-profile-name"
                                           : "settings.widgets.map-placeholders.key"
                  ),
                  .valuePlaceholder = i18n::tr(
                      effectsProfileGlyphs ? "settings.widgets.map-placeholders.glyph-name"
                                           : "settings.widgets.map-placeholders.value"
                  ),
              }
          );
          break;
        }
        case WidgetControlKind::Select: {
          SelectSetting selectSetting;
          const std::string selectedValue = settingValueAsString(value);
          // Font family uses the filterable search picker (catalogs can hold thousands of families).
          if (spec.schema.key == "font_family" && ctx.makeSearchPicker) {
            std::vector<SelectOption> familyOptions;
            familyOptions.reserve(spec.options.size());
            for (const auto& option : spec.options) {
              familyOptions.push_back(
                  SelectOption{option.value, spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
              );
            }
            SearchPickerSetting picker;
            picker.options = std::move(familyOptions);
            picker.selectedValue = selectedValue;
            picker.placeholder = ctx.config.shell.fontFamily;
            picker.emptyText = i18n::tr("ui.controls.search-picker.empty");
            ctx.makeRow(*panel, entry, ctx.makeSearchPicker(picker, entry.title, path));
            break;
          }
          if (spec.optionSource != WidgetSettingOptionSource::Static) {
            selectSetting = sourcedSelectSetting(ctx, spec, selectedValue);
          } else if (spec.schema.key == "font_weight") {
            selectSetting = labelFontWeightSelectSetting(
                spec, widgetLabelFontWeightSelectedValue(ctx.config, widgetName),
                widgetResolvedFontFamily(ctx.config, widgetName)
            );
          } else {
            std::vector<SelectOption> options;
            options.reserve(spec.options.size());
            for (const auto& option : spec.options) {
              options.push_back(
                  SelectOption{option.value, spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
              );
            }
            selectSetting = SelectSetting{std::move(options), selectedValue};
          }
          selectSetting.segmented = spec.segmented;
          selectSetting.valueType = spec.integerValue ? SelectValueType::Integer : SelectValueType::String;
          if (const auto* defaultString = std::get_if<std::string>(&spec.schema.defaultValue);
              defaultString != nullptr) {
            selectSetting.clearOnEmpty = defaultString->empty();
          }
          ctx.makeRow(*panel, entry, ctx.makeSelect(selectSetting, path));
          break;
        }
        case WidgetControlKind::ColorSpec: {
          ColorSpecPickerSetting pickerSetting;
          pickerSetting.selectedValue = settingValueAsString(value);
          pickerSetting.allowNone = spec.advanced;
          pickerSetting.allowCustomColor = spec.allowCustomColor;
          ctx.makeRow(*panel, entry, ctx.makeColorSpecPicker(pickerSetting, path));
          break;
        }
        }
        ++visibleSpecs;
      }

      addRawWidgetSettings(*panel, widgetName, specs, visibleSpecs, ctx);

      if (visibleSpecs == 0) {
        panel->addChild(makeLabel(
            i18n::tr("settings.entities.widget.settings.empty"), Style::fontSizeCaption * ctx.scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
        ));
      }

      item.addChild(std::move(panel));
    }

    void addInspectorPane(Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx) {
      static constexpr std::string_view kLaneKeys[] = {"start", "center", "end"};

      if (ctx.editingWidgetName.empty()) {
        return;
      }

      {
        const std::string widgetName = ctx.editingWidgetName;
        const bool guiManaged = isGuiManagedNamedWidgetInstance(ctx, widgetName);

        std::string currentLaneKey;
        std::vector<std::string> currentLanePath;
        std::vector<std::string> currentLaneItems;
        bool currentLaneInherited = false;
        for (const auto laneKey : kLaneKeys) {
          auto p = pathWithLastSegment(laneListPath, std::string(laneKey));
          auto items = barWidgetItemsForPath(ctx.config, p);
          if (std::ranges::contains(items, widgetName)) {
            currentLaneKey = std::string(laneKey);
            currentLanePath = std::move(p);
            currentLaneItems = std::move(items);
            currentLaneInherited = isMonitorWidgetListPath(currentLanePath)
                && !monitorWidgetListHasExplicitValue(ctx.config, currentLanePath);
            break;
          }
        }

        const std::vector<BarCapsuleGroupStyle> inspectorGroups = capsuleGroupsForLanePath(ctx.config, laneListPath);
        std::string capsuleGroup;
        for (const auto& g : inspectorGroups) {
          if (std::ranges::contains(g.members, widgetName)) {
            capsuleGroup = g.id;
            break;
          }
        }
        if (!capsuleGroup.empty()) {
          auto groupRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale, .fillWidth = true});
          groupRow->addChild(
              makeGlyph("stack-2", Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::Primary))
          );
          auto hint = makeLabel(
              i18n::tr("settings.entities.widget.group.hint"), Style::fontSizeCaption * ctx.scale,
              colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
          );
          hint->setFlexGrow(1.0F);
          groupRow->addChild(std::move(hint));
          const std::string& editGroupId = capsuleGroup;
          groupRow->addChild(
              ui::button({
                  .text = i18n::tr("settings.entities.widget.group.edit"),
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = Style::spaceSm * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [openCapsuleGroupInspector = ctx.openCapsuleGroupInspector, laneListPath, editGroupId]() {
                    if (openCapsuleGroupInspector) {
                      openCapsuleGroupInspector(laneListPath, editGroupId);
                    }
                  },
              })
          );
          body.addChild(std::move(groupRow));
        }

        const bool pendingDelete = ctx.pendingDeleteWidgetName == widgetName;
        const bool renaming = ctx.renamingWidgetName == widgetName;

        if (!currentLaneInherited && !currentLaneKey.empty()) {
          auto actionRow = ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * ctx.scale,
          });

          actionRow->addChild(ui::spacer());

          for (const auto targetLane : kLaneKeys) {
            if (targetLane == currentLaneKey) {
              continue;
            }
            auto sourceItems = currentLaneItems;
            auto sourcePath = currentLanePath;
            auto targetPath = pathWithLastSegment(laneListPath, std::string(targetLane));
            auto targetItems = barWidgetItemsForPath(ctx.config, targetPath);
            actionRow->addChild(
                ui::button({
                    .text = i18n::tr("settings.entities.widget.inspector.move-to-lane", "lane", laneLabel(targetLane)),
                    .fontSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Ghost,
                    .minHeight = Style::controlHeightSm * ctx.scale,
                    .paddingV = Style::spaceXs * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick = [&selectedLaneWidgets = ctx.selectedLaneWidgets, setOverrides = ctx.setOverrides,
                                sourceItems, sourcePath, targetItems, targetPath, widgetName]() mutable {
                      auto it = std::ranges::find(sourceItems, widgetName);
                      if (it == sourceItems.end()) {
                        return;
                      }
                      sourceItems.erase(it);
                      targetItems.push_back(widgetName);
                      selectedLaneWidgets.clear();
                      setOverrides({{sourcePath, sourceItems}, {targetPath, targetItems}});
                    },
                })
            );
          }

          if (guiManaged) {
            actionRow->addChild(
                ui::button({
                    .text = i18n::tr("settings.entities.widget.instance.rename"),
                    .fontSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Ghost,
                    .minHeight = Style::controlHeightSm * ctx.scale,
                    .paddingV = Style::spaceXs * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick = [&renamingWidgetName = ctx.renamingWidgetName,
                                &pendingDeleteWidgetName = ctx.pendingDeleteWidgetName, widgetName,
                                requestRebuild = ctx.requestRebuild]() {
                      renamingWidgetName = widgetName;
                      pendingDeleteWidgetName.clear();
                      requestRebuild();
                    },
                })
            );

            actionRow->addChild(
                ui::button({
                    .text = i18n::tr("settings.entities.widget.instance.delete"),
                    .glyph = "trash",
                    .fontSize = Style::fontSizeCaption * ctx.scale,
                    .glyphSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Ghost,
                    .minHeight = Style::controlHeightSm * ctx.scale,
                    .paddingV = Style::spaceXs * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick = [&pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                                &renamingWidgetName = ctx.renamingWidgetName, widgetName,
                                requestRebuild = ctx.requestRebuild]() {
                      pendingDeleteWidgetName = widgetName;
                      renamingWidgetName.clear();
                      requestRebuild();
                    },
                })
            );
          }

          body.addChild(std::move(actionRow));
        }

        if (renaming) {
          auto renameRow = ui::row({
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * ctx.scale,
          });

          Input* inputPtr = nullptr;
          auto input = ui::input({
              .out = &inputPtr,
              .value = widgetName,
              .placeholder = i18n::tr("settings.entities.widget.instance.id-placeholder"),
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .controlHeight = Style::controlHeightSm * ctx.scale,
              .horizontalPadding = Style::spaceXs * ctx.scale,
              .width = 140.0F * ctx.scale,
              .height = Style::controlHeightSm * ctx.scale,
              .flexGrow = 1.0F,
          });

          auto doRename = [&editingWidgetName = ctx.editingWidgetName, &renamingWidgetName = ctx.renamingWidgetName,
                           config = ctx.config, renameWidgetInstance = ctx.renameWidgetInstance, widgetName,
                           inputPtr](std::string newName) mutable {
            if (!canRenameWidgetInstance(config, widgetName, newName)) {
              inputPtr->setInvalid(true);
              return;
            }
            inputPtr->setInvalid(false);
            auto referenceRenames = widgetReferenceRenameOverrides(config, widgetName, newName);
            renamingWidgetName.clear();
            if (editingWidgetName == widgetName) {
              editingWidgetName = newName;
            }
            renameWidgetInstance(widgetName, std::move(newName), std::move(referenceRenames));
          };

          input->setOnChange([inputPtr](const std::string& /*text*/) { inputPtr->setInvalid(false); });
          input->setOnSubmit([doRename](const std::string& text) mutable { doRename(text); });

          renameRow->addChild(std::move(input));
          renameRow->addChild(
              ui::button({
                  .text = i18n::tr("settings.entities.widget.instance.rename-save"),
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Default,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = Style::spaceSm * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [doRename, inputPtr]() mutable { doRename(inputPtr->value()); },
              })
          );
          renameRow->addChild(
              ui::button({
                  .text = i18n::tr("common.actions.cancel"),
                  .fontSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .minHeight = Style::controlHeightSm * ctx.scale,
                  .paddingV = Style::spaceXs * ctx.scale,
                  .paddingH = Style::spaceSm * ctx.scale,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [&renamingWidgetName = ctx.renamingWidgetName, requestRebuild = ctx.requestRebuild]() {
                    renamingWidgetName.clear();
                    requestRebuild();
                  },
              })
          );
          body.addChild(std::move(renameRow));
        }

        if (pendingDelete) {
          auto confirmPanel = ui::column(
              {
                  .align = FlexAlign::Stretch,
                  .gap = Style::spaceXs * ctx.scale,
                  .padding = Style::spaceSm * ctx.scale,
                  .fill = colorSpecFromRole(ColorRole::Error, 0.10F),
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .border = colorSpecFromRole(ColorRole::Error, 0.5F),
              },
              makeLabel(
                  i18n::tr("settings.entities.widget.instance.delete-confirm-title", "name", widgetName),
                  Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::Error), FontWeight::Bold
              ),
              makeLabel(
                  i18n::tr("settings.entities.widget.instance.delete-confirm-desc"), Style::fontSizeCaption * ctx.scale,
                  colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
              ),
              ui::row(
                  {
                      .align = FlexAlign::Center,
                      .gap = Style::spaceSm * ctx.scale,
                  },
                  ui::spacer(),
                  ui::button({
                      .text = i18n::tr("common.actions.cancel"),
                      .fontSize = Style::fontSizeCaption * ctx.scale,
                      .variant = ButtonVariant::Ghost,
                      .minHeight = Style::controlHeightSm * ctx.scale,
                      .paddingV = Style::spaceXs * ctx.scale,
                      .paddingH = Style::spaceSm * ctx.scale,
                      .radius = Style::scaledRadiusSm(ctx.scale),
                      .onClick =
                          [&pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                           requestRebuild = ctx.requestRebuild]() {
                            pendingDeleteWidgetName.clear();
                            requestRebuild();
                          },
                  }),
                  ui::button({
                      .text = i18n::tr("settings.entities.widget.instance.delete"),
                      .glyph = "trash",
                      .fontSize = Style::fontSizeCaption * ctx.scale,
                      .glyphSize = Style::fontSizeCaption * ctx.scale,
                      .variant = ButtonVariant::Destructive,
                      .minHeight = Style::controlHeightSm * ctx.scale,
                      .paddingV = Style::spaceXs * ctx.scale,
                      .paddingH = Style::spaceSm * ctx.scale,
                      .radius = Style::scaledRadiusSm(ctx.scale),
                      .onClick = [&pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                                  &selectedLaneWidgets = ctx.selectedLaneWidgets, config = ctx.config, widgetName,
                                  clearOverride = ctx.clearOverride, setOverrides = ctx.setOverrides,
                                  closeHostedEditor = ctx.closeHostedEditor]() {
                        pendingDeleteWidgetName.clear();
                        auto referenceRemovals = widgetReferenceRemovalOverrides(config, widgetName);
                        if (!referenceRemovals.empty()) {
                          selectedLaneWidgets.clear();
                          setOverrides(std::move(referenceRemovals));
                        }
                        clearOverride({"widget", widgetName});
                        if (closeHostedEditor) {
                          closeHostedEditor();
                        }
                      },
                  })
              )
          );
          body.addChild(std::move(confirmPanel));
        }

        addWidgetSettingsPanel(body, widgetName, currentLanePath, ctx);

        // Reset to Defaults button — collects all currently overridden setting paths for this widget.
        if (ctx.clearOverrides && ctx.configService != nullptr) {
          const auto widgetType = widgetTypeForReference(ctx.config, widgetName);
          if (!widgetType.empty()) {
            std::vector<std::vector<std::string>> resetPaths;
            const auto widgetIt = ctx.config.widgets.find(widgetName);
            const WidgetConfig* widgetCfg = widgetIt != ctx.config.widgets.end() ? &widgetIt->second : nullptr;
            const noctalia::bar::GestureMask reserved = noctalia::bar::reservedGesturesForType(widgetType);
            auto specs = widgetSettingSpecs(widgetType, widgetCfg, ctx.config.shell.fontFamily);
            for (const auto& spec : specs) {
              if (spec.schema.key == "actions") {
                for (const auto gesture : noctalia::bar::allGestures()) {
                  if (reserved.contains(gesture)) {
                    continue;
                  }
                  std::vector<std::string> gesturePath = {
                      "widget", widgetName, "actions", std::string(noctalia::bar::gestureConfigKey(gesture))
                  };
                  if (ctx.configService->hasEffectiveOverride(gesturePath)) {
                    resetPaths.push_back(std::move(gesturePath));
                  }
                }
              } else {
                auto path = widgetSettingPath(std::string(widgetName), spec.schema.key);
                if (ctx.configService->hasEffectiveOverride(path)) {
                  resetPaths.push_back(std::move(path));
                }
              }
            }
            if (widgetCfg != nullptr) {
              std::set<std::string> knownKeys;
              for (const auto& spec : specs) {
                knownKeys.insert(spec.schema.key);
              }
              for (const auto& [key, value] : widgetCfg->settings) {
                if (knownKeys.contains(key)) {
                  continue;
                }
                auto path = widgetSettingPath(std::string(widgetName), key);
                if (ctx.configService->hasEffectiveOverride(path)) {
                  resetPaths.push_back(std::move(path));
                }
              }
            }
            if (!resetPaths.empty()) {
              body.addChild(
                  ui::row(
                      {
                          .justify = FlexJustify::End,
                          .paddingV = Style::spaceXs * ctx.scale,
                          .fillWidth = true,
                      },
                      ui::button({
                          .text = i18n::tr("settings.entities.widget.inspector.reset-defaults"),
                          .variant = ButtonVariant::Ghost,
                          .onClick = [clearOverrides = ctx.clearOverrides, paths = std::move(resetPaths)]() mutable {
                            clearOverrides(std::move(paths));
                          },
                      })
                  )
              );
            }
          }
        }
      }
    }

    // Color picker control (no label) — placed into a standard settings row via ctx.makeRow.
    std::unique_ptr<Node> makeGroupColorControl(
        const BarWidgetEditorContext& ctx, std::string selectedValue, bool allowNone,
        std::function<void(std::optional<ColorSpec>)> onChange
    ) {
      ColorSpecSelectOptions opts;
      opts.selectedValue = std::move(selectedValue);
      opts.allowNone = allowNone;
      opts.allowCustomColor = true;
      opts.fontSize = Style::fontSizeBody * ctx.scale;
      opts.controlHeight = Style::controlHeight * ctx.scale;
      opts.glyphSize = Style::fontSizeBody * ctx.scale;
      opts.width = 190.0F * ctx.scale;
      return makeColorSpecSelect(
          std::move(opts),
          [onChange](std::string value) { onChange(colorSpecFromConfigString(value, "bar.capsule_group.color")); },
          [onChange]() { onChange(std::nullopt); }
      );
    }

    // Slider + editable numeric value field (no label), matching the shell's standard slider control.
    std::unique_ptr<Node> makeGroupSliderControl(
        const BarWidgetEditorContext& ctx, double value, double minV, double maxV, double step, bool integerValue,
        std::function<void(double)> onCommit
    ) {
      Input* valueInputPtr = nullptr;
      auto valueInput = ui::input({
          .out = &valueInputPtr,
          .value = formatSliderValue(value, integerValue),
          .fontSize = Style::fontSizeCaption * ctx.scale,
          .controlHeight = Style::controlHeightSm * ctx.scale,
          .horizontalPadding = Style::spaceXs * ctx.scale,
          .width = 50.0F * ctx.scale,
          .height = Style::controlHeightSm * ctx.scale,
      });

      Slider* sliderPtr = nullptr;
      auto slider = ui::slider({
          .out = &sliderPtr,
          .minValue = minV,
          .maxValue = maxV,
          .step = step,
          .value = value,
          .trackHeight = Style::sliderTrackHeight * ctx.scale,
          .thumbSize = Style::sliderThumbSize * ctx.scale,
          .controlHeight = Style::controlHeight * ctx.scale,
          .width = Style::sliderDefaultWidth * ctx.scale,
          .height = Style::controlHeight * ctx.scale,
          .onValueChanged = [valueInputPtr, integerValue](double next) {
            valueInputPtr->setInvalid(false);
            valueInputPtr->setValue(formatSliderValue(next, integerValue));
          },
      });
      valueInputPtr->setValue(formatSliderValue(sliderPtr->value(), integerValue));
      slider->setOnDragEnd([sliderPtr, onCommit]() { onCommit(sliderPtr->value()); });

      const auto commitInputText = [sliderPtr, valueInputPtr, minV, maxV, integerValue,
                                    onCommit](const std::string& text) {
        const auto parsed = parseDoubleInput(text);
        if (!parsed.has_value() || *parsed < minV || *parsed > maxV) {
          valueInputPtr->setInvalid(true);
          return false;
        }
        valueInputPtr->setInvalid(false);
        sliderPtr->setValue(*parsed);
        valueInputPtr->setValue(formatSliderValue(sliderPtr->value(), integerValue));
        onCommit(sliderPtr->value());
        return true;
      };
      valueInput->setOnChange([valueInputPtr](const std::string& /*text*/) { valueInputPtr->setInvalid(false); });
      valueInput->setOnSubmit([commitInputText](const std::string& text) { (void)commitInputText(text); });
      valueInput->setOnFocusLoss([commitInputText, valueInputPtr]() { (void)commitInputText(valueInputPtr->value()); });

      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale});
      wrap->addChild(std::move(slider));
      wrap->addChild(std::move(valueInput));
      return wrap;
    }

    // Radius Auto | Custom segmented control + stepper (no label).
    std::unique_ptr<Node> makeGroupRadiusControl(
        const BarWidgetEditorContext& ctx, std::optional<float> radius, float inheritedRadius,
        std::function<void(std::optional<float>)> onChange
    ) {
      const int radiusValue = static_cast<int>(std::lround(std::clamp(radius.value_or(inheritedRadius), 0.0F, 80.0F)));
      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale});
      wrap->addChild(
          ui::segmented({
              .options =
                  std::vector<ui::SegmentedOption>{
                      {.label = i18n::tr("common.states.auto")},
                      {.label = i18n::tr("common.states.custom")},
                  },
              .selectedIndex = static_cast<std::size_t>(radius.has_value() ? 1 : 0),
              .scale = ctx.scale,
              .onChange = [onChange, radiusValue](std::size_t index) {
                onChange(index == 0 ? std::optional<float>{} : std::optional<float>{static_cast<float>(radiusValue)});
              },
          })
      );
      wrap->addChild(
          ui::stepper({
              .minValue = 0,
              .maxValue = 80,
              .step = 1,
              .value = radiusValue,
              .enabled = radius.has_value(),
              .scale = ctx.scale,
              .onValueCommitted = [onChange](int v) { onChange(std::optional<float>{static_cast<float>(v)}); },
          })
      );
      return wrap;
    }

    // Spacing Auto | Custom segmented control + stepper (no label). "Auto" inherits the bar's widget spacing.
    std::unique_ptr<Node> makeGroupSpacingControl(
        const BarWidgetEditorContext& ctx, std::optional<std::int32_t> spacing, std::int32_t inherited,
        std::function<void(std::optional<std::int32_t>)> onChange
    ) {
      const int spacingValue = std::clamp(static_cast<int>(spacing.value_or(inherited)), 0, 32);
      auto wrap = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale});
      wrap->addChild(
          ui::segmented({
              .options =
                  std::vector<ui::SegmentedOption>{
                      {.label = i18n::tr("common.states.auto")},
                      {.label = i18n::tr("common.states.custom")},
                  },
              .selectedIndex = static_cast<std::size_t>(spacing.has_value() ? 1 : 0),
              .scale = ctx.scale,
              .onChange = [onChange, spacingValue](std::size_t index) {
                onChange(
                    index == 0 ? std::optional<std::int32_t>{}
                               : std::optional<std::int32_t>{static_cast<std::int32_t>(spacingValue)}
                );
              },
          })
      );
      wrap->addChild(
          ui::stepper({
              .minValue = 0,
              .maxValue = 32,
              .step = 1,
              .value = spacingValue,
              .enabled = spacing.has_value(),
              .scale = ctx.scale,
              .onValueCommitted = [onChange](int v) {
                onChange(std::optional<std::int32_t>{static_cast<std::int32_t>(v)});
              },
          })
      );
      return wrap;
    }

    void addCapsuleGroupInspector(
        Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx
    ) {
      if (ctx.editingCapsuleGroupId.empty() || laneListPath.size() < 2 || laneListPath[0] != "bar") {
        return;
      }
      const std::string groupId = ctx.editingCapsuleGroupId;
      const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(laneListPath);
      if (groupPath.empty()) {
        return;
      }
      const std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(ctx.config, laneListPath);
      const BarCapsuleGroupStyle* stylePtr = findCapsuleGroupStyle(groups, groupId);
      if (stylePtr == nullptr) {
        if (ctx.closeHostedEditor) {
          ctx.closeHostedEditor();
        }
        return;
      }
      const BarCapsuleGroupStyle style = *stylePtr;
      const std::size_t memberCount = stylePtr->members.size();

      body.addChild(makeSettingSubtitleLabel(
          i18n::tr("settings.entities.widget.group.members", "count", std::to_string(memberCount)), ctx.scale
      ));

      // Commits a mutated copy of the group style vector.
      const auto mutateGroup = [setOverride = ctx.setOverride, groups, groupPath,
                                groupId](const std::function<void(BarCapsuleGroupStyle&)>& fn) {
        std::vector<BarCapsuleGroupStyle> updated = groups;
        for (auto& g : updated) {
          if (g.id == groupId) {
            fn(g);
            break;
          }
        }
        setOverride(groupPath, updated);
      };

      // Controls use the standard settings row (ctx.makeRow), matching the per-widget settings editor.
      auto panel = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * ctx.scale,
      });
      Flex* panelPtr = panel.get();

      const auto groupEntry = [&](std::string_view field) {
        const std::string base = std::string("settings.entities.widget.group.") + std::string(field);
        std::vector<std::string> fieldPath = groupPath;
        fieldPath.push_back(groupId);
        fieldPath.emplace_back(field);
        return SettingEntry{
            .section = SettingsSection::Bar,
            .group = "capsule-group",
            .title = i18n::tr(base),
            .subtitle = i18n::tr(base + "-description"),
            .path = std::move(fieldPath),
            .control = {},
            .searchText = {},
        };
      };

      panel->addChild(makeMiniSectionHeader(i18n::tr("settings.entities.widget.group.style"), ctx.scale, false));

      ctx.makeRow(
          *panelPtr, groupEntry("fill"),
          makeGroupColorControl(
              ctx, colorSpecConfigValue(style.fill), false, [mutateGroup](std::optional<ColorSpec> c) {
                if (c.has_value()) {
                  mutateGroup([&](BarCapsuleGroupStyle& g) { g.fill = *c; });
                }
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("border"),
          makeGroupColorControl(
              ctx, optionalColorSpecConfigValue(style.border), true, [mutateGroup](std::optional<ColorSpec> c) {
                mutateGroup([&](BarCapsuleGroupStyle& g) {
                  g.borderSpecified = true;
                  g.border = c;
                });
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("foreground"),
          makeGroupColorControl(
              ctx, optionalColorSpecConfigValue(style.foreground), true, [mutateGroup](std::optional<ColorSpec> c) {
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.foreground = c; });
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("padding"),
          makeGroupSliderControl(
              ctx, static_cast<double>(style.padding), 0.0, 48.0, 1.0, true, [mutateGroup](double v) {
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.padding = static_cast<float>(v); });
              }
          )
      );
      const BarConfig* laneBar = barForLanePath(ctx.config, laneListPath);
      const BarMonitorOverride* laneOvr = monitorOverrideForLanePath(ctx.config, laneListPath);
      // "Auto" inherits the spacing this lane actually resolves to, monitor override included.
      const std::int32_t inheritedSpacing = laneOvr != nullptr && laneOvr->widgetSpacing.has_value()
          ? *laneOvr->widgetSpacing
          : (laneBar != nullptr ? laneBar->widgetSpacing : 6);
      ctx.makeRow(
          *panelPtr, groupEntry("widget-spacing"),
          makeGroupSpacingControl(
              ctx, style.widgetSpacing, inheritedSpacing, [mutateGroup](std::optional<std::int32_t> s) {
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.widgetSpacing = s; });
              }
          )
      );
      ctx.makeRow(
          *panelPtr, groupEntry("opacity"),
          makeGroupSliderControl(
              ctx, static_cast<double>(style.opacity), 0.0, 1.0, 0.05, false, [mutateGroup](double v) {
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.opacity = static_cast<float>(v); });
              }
          )
      );

      const auto inheritedRadius = static_cast<float>(inheritedCapsuleRadiusForLane(ctx.config, laneListPath));

      ctx.makeRow(
          *panelPtr, groupEntry("radius"),
          makeGroupRadiusControl(ctx, style.radius, inheritedRadius, [mutateGroup](std::optional<float> r) {
            mutateGroup([&](BarCapsuleGroupStyle& g) { g.radius = r; });
          })
      );
      const BarCapsuleContour inheritedContour = laneOvr != nullptr && laneOvr->widgetCapsuleContour.has_value()
          ? *laneOvr->widgetCapsuleContour
          : (laneBar != nullptr ? laneBar->widgetCapsuleContour : BarCapsuleContour::Rounded);
      const BarCapsuleContour contour = style.contour.value_or(inheritedContour);
      ctx.makeRow(
          *panelPtr, groupEntry("contour"),
          ui::segmented({
              .options = std::vector<ui::SegmentedOption>{
                  {.label = i18n::tr("settings.options.capsule-contour.rounded")},
                  {.label = i18n::tr("settings.options.capsule-contour.powerline")},
                  {.label = i18n::tr("settings.options.capsule-contour.powerline-start")},
                  {.label = i18n::tr("settings.options.capsule-contour.powerline-end")},
              },
              .selectedIndex = static_cast<std::size_t>(contour),
              .scale = ctx.scale,
              .onChange = [mutateGroup](std::size_t index) {
                const auto next = static_cast<BarCapsuleContour>(std::min<std::size_t>(index, 3));
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.contour = next; });
              },
          })
      );
      if (contour != BarCapsuleContour::Rounded) {
        const float inheritedDepth = laneOvr != nullptr && laneOvr->widgetCapsuleContourDepth.has_value()
            ? static_cast<float>(*laneOvr->widgetCapsuleContourDepth)
            : (laneBar != nullptr ? laneBar->widgetCapsuleContourDepth : 8.0F);
        ctx.makeRow(
            *panelPtr, groupEntry("contour-depth"),
            makeGroupSliderControl(
                ctx, style.contourDepth.value_or(inheritedDepth), 0.0, 32.0, 1.0, true,
                [mutateGroup](double value) {
                  mutateGroup([&](BarCapsuleGroupStyle& g) { g.contourDepth = static_cast<float>(value); });
                }
            )
        );
      }
      ctx.makeRow(
          *panelPtr, groupEntry("accordion"),
          ui::toggle({
              .checked = style.accordion,
              .scale = ctx.scale,
              .onChange = [mutateGroup](bool checked) {
                mutateGroup([&](BarCapsuleGroupStyle& g) { g.accordion = checked; });
              },
          })
      );
      // Direction only matters while accordion is on; the inspector rebuilds when the toggle commits.
      if (style.accordion) {
        ctx.makeRow(
            *panelPtr, groupEntry("accordion-direction"),
            ui::segmented({
                .options =
                    std::vector<ui::SegmentedOption>{
                        {.label = i18n::tr("settings.options.accordion-direction.end")},
                        {.label = i18n::tr("settings.options.accordion-direction.start")},
                    },
                .selectedIndex =
                    static_cast<std::size_t>(style.accordionDirection == BarAccordionDirection::Start ? 1 : 0),
                .scale = ctx.scale,
                .onChange = [mutateGroup](std::size_t index) {
                  mutateGroup([&](BarCapsuleGroupStyle& g) {
                    g.accordionDirection = index == 1 ? BarAccordionDirection::Start : BarAccordionDirection::End;
                  });
                },
            })
        );
      }

      body.addChild(std::move(panel));
      body.addChild(
          ui::button({
              .text = i18n::tr("settings.entities.widget.group.ungroup"),
              .glyph = "stack-pop",
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .glyphSize = Style::fontSizeCaption * ctx.scale,
              .variant = ButtonVariant::Default,
              .minHeight = Style::controlHeightSm * ctx.scale,
              .paddingV = Style::spaceXs * ctx.scale,
              .paddingH = Style::spaceSm * ctx.scale,
              .radius = Style::scaledRadiusSm(ctx.scale),
              .onClick = [&selectedLaneWidgets = ctx.selectedLaneWidgets, setOverrides = ctx.setOverrides, groupId,
                          groupPath, laneListPath, config = &ctx.config, closeHostedEditor = ctx.closeHostedEditor]() {
                std::vector<BarCapsuleGroupStyle> currentGroups = capsuleGroupsForLanePath(*config, laneListPath);
                const BarCapsuleGroupStyle* g = findCapsuleGroupStyle(currentGroups, groupId);
                if (g == nullptr) {
                  if (closeHostedEditor) {
                    closeHostedEditor();
                  }
                  return;
                }
                if (groupPath.empty()) {
                  return;
                }
                const std::vector<std::string> members = g->members;
                std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> batch;
                // Replace the group token with its members in whichever lane holds it.
                const std::string token = makeCapsuleGroupToken(groupId);
                for (const auto laneKey : {"start", "center", "end"}) {
                  std::vector<std::string> lanePath = pathWithLastSegment(laneListPath, laneKey);
                  std::vector<std::string> lane = barWidgetItemsForPath(*config, lanePath);
                  const auto it = std::ranges::find(lane, token);
                  if (it == lane.end()) {
                    continue;
                  }
                  const std::size_t pos = static_cast<std::size_t>(it - lane.begin());
                  lane.erase(lane.begin() + static_cast<std::ptrdiff_t>(pos));
                  lane.insert(lane.begin() + static_cast<std::ptrdiff_t>(pos), members.begin(), members.end());
                  batch.emplace_back(lanePath, lane);
                }
                std::vector<BarCapsuleGroupStyle> remaining;
                for (const auto& gx : currentGroups) {
                  if (gx.id != groupId) {
                    remaining.push_back(gx);
                  }
                }
                batch.emplace_back(groupPath, remaining);
                selectedLaneWidgets.clear();
                setOverrides(std::move(batch));
                if (closeHostedEditor) {
                  closeHostedEditor();
                }
              },
          })
      );
    }

    struct LaneGroupPlan {
      bool groupable = false;
      std::string laneKey;
      std::vector<std::size_t> indices; // selected lane positions, ascending
      std::vector<std::string> members; // selection in lane order
    };

    // Selection tokens are "<laneKey>#<index>". Grouping needs ≥2 selected widgets in one lane, none of
    // which is already a group token. They need not be adjacent: the group lands on the first one.
    LaneGroupPlan computeLaneGroupPlan(const SettingEntry& entry, const BarWidgetEditorContext& ctx) {
      LaneGroupPlan plan;
      const auto& selection = ctx.selectedLaneWidgets;
      if (selection.size() < 2) {
        return plan;
      }
      std::string laneKey;
      std::vector<std::size_t> indices;
      for (const auto& token : selection) {
        const auto parsed = parseLaneSelectionToken(token);
        if (!parsed.has_value()) {
          return plan;
        }
        if (laneKey.empty()) {
          laneKey = parsed->laneKey;
        } else if (laneKey != parsed->laneKey) {
          return plan; // selection spans multiple lanes
        }
        indices.push_back(parsed->index);
      }
      std::ranges::sort(indices);

      const std::vector<std::string> items =
          barWidgetItemsForPath(ctx.config, pathWithLastSegment(entry.path, laneKey));
      for (const auto idx : indices) {
        if (idx >= items.size() || isCapsuleGroupToken(items[idx])) {
          return plan;
        }
        plan.members.push_back(items[idx]);
      }
      plan.groupable = true;
      plan.laneKey = laneKey;
      plan.indices = std::move(indices);
      return plan;
    }

    void addLaneSelectionToolbar(Flex& block, const SettingEntry& entry, const BarWidgetEditorContext& ctx) {
      const LaneGroupPlan plan = computeLaneGroupPlan(entry, ctx);

      auto toolbar = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceSm * ctx.scale,
          .paddingV = Style::spaceXs * ctx.scale,
          .paddingH = Style::spaceSm * ctx.scale,
          .fill = colorSpecFromRole(ColorRole::Primary, 0.12F),
          .radius = Style::scaledRadiusSm(ctx.scale),
          .fillWidth = true,
      });
      auto label = makeLabel(
          i18n::tr("settings.entities.widget.group.selected", "count", std::to_string(ctx.selectedLaneWidgets.size())),
          Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::Bold
      );
      label->setFlexGrow(1.0F);
      toolbar->addChild(std::move(label));

      if (plan.groupable) {
        toolbar->addChild(
            ui::button({
                .text = i18n::tr("settings.entities.widget.group.action"),
                .glyph = "stack-2",
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Primary,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = [setOverrides = ctx.setOverrides, config = &ctx.config, laneKey = plan.laneKey,
                            indices = plan.indices, members = plan.members, laneListPath = entry.path,
                            &selectedLaneWidgets = ctx.selectedLaneWidgets,
                            openCapsuleGroupInspector = ctx.openCapsuleGroupInspector]() {
                  const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(laneListPath);
                  if (groupPath.empty() || indices.empty()) {
                    return;
                  }
                  std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(*config, laneListPath);
                  const std::string newId = nextCapsuleGroupId(groups);
                  BarCapsuleGroupStyle newGroup = seedCapsuleGroupStyle(*config, laneListPath, newId);
                  newGroup.members = members;
                  groups.push_back(std::move(newGroup));

                  // Pull the selected widgets out of the lane, wherever they sit, and leave one group
                  // token at the first of them. Unselected widgets in between keep their order.
                  std::vector<std::string> lanePath = pathWithLastSegment(laneListPath, laneKey);
                  const std::vector<std::string> lane = barWidgetItemsForPath(*config, lanePath);
                  if (indices.back() >= lane.size()) {
                    return;
                  }
                  std::vector<std::string> nextLane;
                  nextLane.reserve(lane.size());
                  for (std::size_t i = 0; i < lane.size(); ++i) {
                    if (i == indices.front()) {
                      nextLane.push_back(makeCapsuleGroupToken(newId));
                    }
                    if (!std::ranges::binary_search(indices, i)) {
                      nextLane.push_back(lane[i]);
                    }
                  }

                  selectedLaneWidgets.clear();
                  setOverrides({{lanePath, nextLane}, {groupPath, groups}});
                  if (openCapsuleGroupInspector) {
                    openCapsuleGroupInspector(laneListPath, newId);
                  }
                },
            })
        );
      }
      toolbar->addChild(
          ui::button({
              .text = i18n::tr("settings.entities.widget.group.clear"),
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .variant = ButtonVariant::Ghost,
              .minHeight = Style::controlHeightSm * ctx.scale,
              .paddingV = Style::spaceXs * ctx.scale,
              .paddingH = Style::spaceSm * ctx.scale,
              .radius = Style::scaledRadiusSm(ctx.scale),
              .onClick = [&selectedLaneWidgets = ctx.selectedLaneWidgets, requestRebuild = ctx.requestRebuild]() {
                selectedLaneWidgets.clear();
                requestRebuild();
              },
          })
      );
      block.addChild(std::move(toolbar));
    }

    std::vector<std::string> sectionOverridePath(const std::vector<std::string>& lanePath) {
      std::vector<std::string> path = lanePath;
      if (!path.empty()) {
        path.back() = "section";
      }
      return path;
    }

    std::vector<BarSectionConfig>
    sectionsForLanePath(const Config& cfg, const std::vector<std::string>& lanePath, bool* inherited = nullptr) {
      if (inherited != nullptr) {
        *inherited = false;
      }
      if (lanePath.size() < 3 || lanePath[0] != "bar") {
        return {};
      }
      const BarConfig* bar = findBar(cfg, lanePath[1]);
      if (bar == nullptr) {
        return {};
      }
      if (isMonitorWidgetListPath(lanePath)) {
        const BarMonitorOverride* monitor = findMonitorOverride(*bar, lanePath[3]);
        if (monitor != nullptr && monitor->sectionsSpecified) {
          return monitor->sections;
        }
        if (inherited != nullptr) {
          *inherited = !bar->sections.empty();
        }
      }
      return bar->sections;
    }

    std::vector<BarSectionConfig> legacySectionsForLanePath(
        const Config& cfg, const std::vector<std::string>& lanePath
    ) {
      const BarConfig* bar = lanePath.size() >= 2 ? findBar(cfg, lanePath[1]) : nullptr;
      const BarCenterAlignment centerAlignment = bar != nullptr ? bar->centerAlignment : BarCenterAlignment::Center;
      std::vector<BarSectionConfig> sections;
      sections.push_back({
          .id = "start",
          .widgets = barWidgetItemsForPath(cfg, pathWithLastSegment(lanePath, "start")),
          .anchor = BarCenterAlignment::Start,
          .alignment = BarCenterAlignment::Start,
      });
      sections.push_back({
          .id = "center",
          .widgets = barWidgetItemsForPath(cfg, pathWithLastSegment(lanePath, "center")),
          .anchor = BarCenterAlignment::Center,
          .alignment = centerAlignment,
      });
      sections.push_back({
          .id = "end",
          .widgets = barWidgetItemsForPath(cfg, pathWithLastSegment(lanePath, "end")),
          .anchor = BarCenterAlignment::End,
          .alignment = BarCenterAlignment::End,
      });
      return sections;
    }

    std::string nextSectionId(const std::vector<BarSectionConfig>& sections) {
      for (std::size_t suffix = sections.size() + 1;; ++suffix) {
        const std::string candidate = "section-" + std::to_string(suffix);
        if (std::ranges::none_of(sections, [&candidate](const BarSectionConfig& section) {
              return section.id == candidate;
            })) {
          return candidate;
        }
      }
    }

    std::string trimSectionText(std::string_view text) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
      }
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
      }
      return std::string(text);
    }

    void addSectionControl(
        Flex& card, std::string_view title, std::string_view subtitle, std::unique_ptr<Node> control, float scale
    ) {
      auto row = ui::row({
          .align = FlexAlign::Center,
          .gap = Style::spaceMd * scale,
          .paddingV = Style::spaceXs * scale,
          .fillWidth = true,
      });
      auto labels = ui::column({.align = FlexAlign::Stretch, .gap = 1.0F * scale, .flexGrow = 1.0F});
      labels->addChild(makeLabel(
          title, Style::fontSizeCaption * scale, colorSpecFromRole(ColorRole::OnSurface), FontWeight::SemiBold
      ));
      if (!subtitle.empty()) {
        labels->addChild(makeLabel(
            subtitle, Style::fontSizeMini * scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
        ));
      }
      row->addChild(std::move(labels));
      row->addChild(std::move(control));
      card.addChild(std::move(row));
    }

    std::unique_ptr<Node> makeOptionalSectionSlider(
        const BarWidgetEditorContext& ctx, std::optional<float> configured, float inherited, double minValue,
        double maxValue, double step, std::function<void(std::optional<float>)> onChange
    ) {
      const float shown = std::clamp(configured.value_or(inherited), static_cast<float>(minValue), static_cast<float>(maxValue));
      auto control = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale});
      control->addChild(ui::segmented({
          .options = std::vector<ui::SegmentedOption>{{.label = "Inherit"}, {.label = "Custom"}},
          .selectedIndex = configured.has_value() ? 1U : 0U,
          .scale = ctx.scale,
          .onChange = [onChange, shown](std::size_t index) {
            onChange(index == 0 ? std::optional<float>{} : std::optional<float>{shown});
          },
      }));
      control->addChild(makeGroupSliderControl(
          ctx, shown, minValue, maxValue, step, false,
          [onChange](double value) { onChange(static_cast<float>(value)); }
      ));
      return control;
    }

    void addNamedSectionEditor(
        Flex& block, const SettingEntry& entry, const BarWidgetEditorContext& ctx,
        const std::vector<BarSectionConfig>& sections, bool inherited
    ) {
      const std::vector<std::string> overridePath = sectionOverridePath(entry.path);
      block.addChild(makeLabel(
          inherited ? "These sections are inherited from the base bar. Editing creates a monitor-specific copy."
                    : "Sections are ordered independently. Each can be anchored, offset, styled, and bound to actions.",
          Style::fontSizeMini * ctx.scale, colorSpecFromRole(ColorRole::OnSurfaceVariant)
      ));

      const auto commit = [setOverride = ctx.setOverride, overridePath](std::vector<BarSectionConfig> next) {
        setOverride(overridePath, std::move(next));
      };
      const BarConfig* owningBar = entry.path.size() >= 2 ? findBar(ctx.config, entry.path[1]) : nullptr;
      const std::vector<BarWidgetPlacementConfig> currentPlacements =
          owningBar != nullptr ? owningBar->widgetPlacements : std::vector<BarWidgetPlacementConfig>{};
      const auto appendWidget = [setOverrides = ctx.setOverrides, overridePath,
                                 placementPath = std::vector<std::string>{"bar", entry.path[1], "widget_placement"},
                                 barName = entry.path[1], currentPlacements](
                                    std::vector<BarSectionConfig> next, std::size_t index, std::string widget
                                 ) {
        auto placements = currentPlacements;
        std::unordered_set<std::string> occupied;
        for (const auto& placement : placements) occupied.insert(placement.id);
        std::string id = StringUtils::generateUuid();
        if (id.empty() || occupied.contains(id)) {
          id = noctalia::bar::legacyBarWidgetPlacementId(
              barName, next[index].id, widget, next[index].widgets.size(), occupied
          );
        }
        placements.push_back({.id = id, .widget = std::move(widget)});
        next[index].widgets.push_back(noctalia::bar::makeBarWidgetPlacementToken(id));
        setOverrides({{placementPath, placements}, {overridePath, next}});
      };

      for (std::size_t index = 0; index < sections.size(); ++index) {
        const BarSectionConfig current = sections[index];
        auto card = ui::column({
            .align = FlexAlign::Stretch,
            .gap = Style::spaceXs * ctx.scale,
            .padding = Style::spaceSm * ctx.scale,
            .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.42F),
            .radius = Style::scaledRadiusMd(ctx.scale),
            .border = colorSpecFromRole(ColorRole::Outline),
            .fillWidth = true,
        });

        auto header = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale, .fillWidth = true});
        Input* idInputPtr = nullptr;
        auto idInput = ui::input({
            .out = &idInputPtr,
            .value = current.id,
            .placeholder = "Unique section name",
            .fontSize = Style::fontSizeBody * ctx.scale,
            .controlHeight = Style::controlHeightSm * ctx.scale,
            .horizontalPadding = Style::spaceSm * ctx.scale,
            .flexGrow = 1.0F,
            .onChange = [idInputPtr](const std::string&) { idInputPtr->setInvalid(false); },
            .onSubmit = [idInputPtr, sections = std::vector<BarSectionConfig>(sections), index, commit](const std::string& text) {
              const std::string id = trimSectionText(text);
              const bool duplicate = std::ranges::any_of(sections, [index, &id, i = std::size_t{0}](const BarSectionConfig& section) mutable {
                return i++ != index && section.id == id;
              });
              if (id.empty() || duplicate) {
                idInputPtr->setInvalid(true);
                return;
              }
              auto next = sections;
              next[index].id = id;
              commit(std::move(next));
            },
            .submitOnFocusLoss = true,
        });
        header->addChild(std::move(idInput));
        header->addChild(ui::button({
            .glyph = "chevron-up",
            .glyphSize = Style::fontSizeCaption * ctx.scale,
            .enabled = index > 0,
            .variant = ButtonVariant::Ghost,
            .tooltip = "Move section earlier",
            .minWidth = Style::controlHeightSm * ctx.scale,
            .minHeight = Style::controlHeightSm * ctx.scale,
            .padding = Style::spaceXs * ctx.scale,
            .onClick = [sections = std::vector<BarSectionConfig>(sections), index, commit]() mutable {
              if (index > 0) {
                std::swap(sections[index - 1], sections[index]);
                commit(std::move(sections));
              }
            },
        }));
        header->addChild(ui::button({
            .glyph = "chevron-down",
            .glyphSize = Style::fontSizeCaption * ctx.scale,
            .enabled = index + 1 < sections.size(),
            .variant = ButtonVariant::Ghost,
            .tooltip = "Move section later",
            .minWidth = Style::controlHeightSm * ctx.scale,
            .minHeight = Style::controlHeightSm * ctx.scale,
            .padding = Style::spaceXs * ctx.scale,
            .onClick = [sections = std::vector<BarSectionConfig>(sections), index, commit]() mutable {
              if (index + 1 < sections.size()) {
                std::swap(sections[index], sections[index + 1]);
                commit(std::move(sections));
              }
            },
        }));
        header->addChild(ui::button({
            .glyph = "trash",
            .glyphSize = Style::fontSizeCaption * ctx.scale,
            .variant = ButtonVariant::Ghost,
            .tooltip = "Remove section",
            .minWidth = Style::controlHeightSm * ctx.scale,
            .minHeight = Style::controlHeightSm * ctx.scale,
            .padding = Style::spaceXs * ctx.scale,
            .onClick = [sections = std::vector<BarSectionConfig>(sections), index, commit]() mutable {
              sections.erase(sections.begin() + static_cast<std::ptrdiff_t>(index));
              commit(std::move(sections));
            },
        }));
        card->addChild(std::move(header));

        addSectionControl(*card, "Anchor", "Position along the bar", ui::segmented({
            .options = std::vector<ui::SegmentedOption>{{.label = "Start"}, {.label = "Center"}, {.label = "End"}},
            .selectedIndex = static_cast<std::size_t>(current.anchor),
            .scale = ctx.scale,
            .onChange = [sections = std::vector<BarSectionConfig>(sections), index, commit](std::size_t selected) mutable {
              sections[index].anchor = static_cast<BarCenterAlignment>(std::min(selected, std::size_t{2}));
              commit(std::move(sections));
            },
        }), ctx.scale);
        addSectionControl(*card, "Content alignment", "Alignment around the anchor", ui::segmented({
            .options = std::vector<ui::SegmentedOption>{{.label = "Start"}, {.label = "Center"}, {.label = "End"}},
            .selectedIndex = static_cast<std::size_t>(current.alignment),
            .scale = ctx.scale,
            .onChange = [sections = std::vector<BarSectionConfig>(sections), index, commit](std::size_t selected) mutable {
              sections[index].alignment = static_cast<BarCenterAlignment>(std::min(selected, std::size_t{2}));
              commit(std::move(sections));
            },
        }), ctx.scale);
        addSectionControl(*card, "Layout participation", "Free keeps explicit placement; lane roles follow the bar's center and edge policies", ui::segmented({
            .options = std::vector<ui::SegmentedOption>{{.label = "Free"}, {.label = "Start lane"}, {.label = "Center lane"}, {.label = "End lane"}},
            .selectedIndex = static_cast<std::size_t>(current.layoutRole),
            .scale = ctx.scale,
            .onChange = [sections = std::vector<BarSectionConfig>(sections), index, commit](std::size_t selected) mutable {
              sections[index].layoutRole = static_cast<BarSectionLayoutRole>(std::min(selected, std::size_t{3}));
              commit(std::move(sections));
            },
        }), ctx.scale);
        addSectionControl(*card, "Main-axis offset", "Logical pixels along the bar", makeGroupSliderControl(
            ctx, current.offset, -500.0, 500.0, 1.0, false,
            [sections = std::vector<BarSectionConfig>(sections), index, commit](double value) mutable {
              sections[index].offset = static_cast<float>(value);
              commit(std::move(sections));
            }
        ), ctx.scale);
        addSectionControl(*card, "Cross-axis offset", "Positive values move toward the desktop", makeGroupSliderControl(
            ctx, current.crossOffset, -200.0, 200.0, 1.0, false,
            [sections = std::vector<BarSectionConfig>(sections), index, commit](double value) mutable {
              sections[index].crossOffset = static_cast<float>(value);
              commit(std::move(sections));
            }
        ), ctx.scale);
        addSectionControl(*card, "Allow overlap", "Permit this section to cross another section", ui::toggle({
            .checked = current.allowOverlap,
            .scale = ctx.scale,
            .onChange = [sections = std::vector<BarSectionConfig>(sections), index, commit](bool enabled) mutable {
              sections[index].allowOverlap = enabled;
              commit(std::move(sections));
            },
        }), ctx.scale);

        addSectionControl(*card, "Material", "Inherit the bar or choose a native surface", ui::segmented({
            .options = std::vector<ui::SegmentedOption>{{.label = "Inherit"}, {.label = "Solid"}, {.label = "Glass"}, {.label = "Clear"}},
            .selectedIndex = static_cast<std::size_t>(current.materialMode),
            .scale = ctx.scale,
            .onChange = [sections = std::vector<BarSectionConfig>(sections), index, commit](std::size_t selected) mutable {
              sections[index].materialMode = static_cast<BarMaterialMode>(std::min(selected, std::size_t{3}));
              commit(std::move(sections));
            },
        }), ctx.scale);
        addSectionControl(*card, "Shader", "Native section shader preset", ui::segmented({
            .options = std::vector<ui::SegmentedOption>{{.label = "Inherit"}, {.label = "Flat"}, {.label = "Optical"}},
            .selectedIndex = static_cast<std::size_t>(current.shader),
            .scale = ctx.scale,
            .onChange = [sections = std::vector<BarSectionConfig>(sections), index, commit](std::size_t selected) mutable {
              sections[index].shader = static_cast<BarSectionShader>(std::min(selected, std::size_t{2}));
              commit(std::move(sections));
            },
        }), ctx.scale);
        addSectionControl(*card, "Background", "Unset inherits the bar background", makeGroupColorControl(
            ctx, current.background.has_value() ? colorSpecToConfigString(*current.background) : std::string{}, true,
            [sections = std::vector<BarSectionConfig>(sections), index, commit](std::optional<ColorSpec> color) mutable {
              sections[index].background = std::move(color);
              commit(std::move(sections));
            }
        ), ctx.scale);
        addSectionControl(*card, "Background opacity", "Optional section opacity multiplier", makeOptionalSectionSlider(
            ctx, current.backgroundOpacity, 1.0F, 0.0, 1.0, 0.05,
            [sections = std::vector<BarSectionConfig>(sections), index, commit](std::optional<float> value) mutable {
              sections[index].backgroundOpacity = value;
              commit(std::move(sections));
            }
        ), ctx.scale);
        addSectionControl(*card, "Border", "Unset inherits the bar outline", makeGroupColorControl(
            ctx, current.border.has_value() ? colorSpecToConfigString(*current.border) : std::string{}, true,
            [sections = std::vector<BarSectionConfig>(sections), index, commit](std::optional<ColorSpec> color) mutable {
              sections[index].border = std::move(color);
              commit(std::move(sections));
            }
        ), ctx.scale);
        addSectionControl(*card, "Border width", "Optional section outline width", makeOptionalSectionSlider(
            ctx, current.borderWidth, 0.0F, 0.0, 12.0, 0.5,
            [sections = std::vector<BarSectionConfig>(sections), index, commit](std::optional<float> value) mutable {
              sections[index].borderWidth = value;
              commit(std::move(sections));
            }
        ), ctx.scale);

        card->addChild(ui::separator());
        card->addChild(makeLabel(
            "Widgets", Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold
        ));
        for (std::size_t widgetIndex = 0; widgetIndex < current.widgets.size(); ++widgetIndex) {
          const std::string widgetEntry = current.widgets[widgetIndex];
          const auto resolvedWidget = resolveBarWidgetEntry(ctx.config, entry.path, widgetEntry);
          const std::string& widgetName = resolvedWidget.first;
          const auto info = widgetReferenceInfo(ctx.config, widgetName, false);
          auto widgetRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale, .fillWidth = true});
          auto widgetLabel = makeLabel(
              info.title, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface)
          );
          widgetLabel->setFlexGrow(1.0F);
          widgetRow->addChild(std::move(widgetLabel));
          if (!widgetTypeForReference(ctx.config, widgetName).empty()) {
            widgetRow->addChild(ui::button({
                .glyph = "settings",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = "Configure widget",
                .minWidth = Style::controlHeightSm * ctx.scale,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .padding = Style::spaceXs * ctx.scale,
                .onClick = [openWidgetInspector = ctx.openWidgetInspector, lanePath = entry.path, widgetName]() {
                  if (openWidgetInspector) openWidgetInspector(lanePath, widgetName);
                },
            }));
          }
          if (ctx.showAdvanced && !resolvedWidget.second.empty()) {
            const std::string target = noctalia::bar::barWidgetMaterialTarget(resolvedWidget.second);
            const auto found = ctx.config.shell.materialOverrides.surfaces.find(target);
            SelectSetting material;
            material.options = {
                {.value = "", .label = "Inherit"},
                {.value = "flat", .label = "Flat"},
                {.value = "neumorphic", .label = "Raised plateau"},
                {.value = "liquid_glass", .label = "Optical glass"},
                {.value = "illustrated", .label = "Illustrated"},
            };
            material.selectedValue = found != ctx.config.shell.materialOverrides.surfaces.end()
                    && found->second.primitive
                ? std::string(Style::materialPrimitiveName(*found->second.primitive)) : std::string{};
            material.clearOnEmpty = true;
            widgetRow->addChild(ctx.makeSelect(
                material, {"shell", "material_overrides", "surfaces", target, "primitive"}
            ));
          }
          widgetRow->addChild(ui::button({
              .glyph = "chevron-up",
              .glyphSize = Style::fontSizeCaption * ctx.scale,
              .enabled = widgetIndex > 0,
              .variant = ButtonVariant::Ghost,
              .tooltip = "Move widget earlier",
              .minWidth = Style::controlHeightSm * ctx.scale,
              .minHeight = Style::controlHeightSm * ctx.scale,
              .padding = Style::spaceXs * ctx.scale,
              .onClick = [sections = std::vector<BarSectionConfig>(sections), index, widgetIndex, commit]() mutable {
                if (widgetIndex > 0) {
                  std::swap(sections[index].widgets[widgetIndex - 1], sections[index].widgets[widgetIndex]);
                  commit(std::move(sections));
                }
              },
          }));
          widgetRow->addChild(ui::button({
              .glyph = "chevron-down",
              .glyphSize = Style::fontSizeCaption * ctx.scale,
              .enabled = widgetIndex + 1 < current.widgets.size(),
              .variant = ButtonVariant::Ghost,
              .tooltip = "Move widget later",
              .minWidth = Style::controlHeightSm * ctx.scale,
              .minHeight = Style::controlHeightSm * ctx.scale,
              .padding = Style::spaceXs * ctx.scale,
              .onClick = [sections = std::vector<BarSectionConfig>(sections), index, widgetIndex, commit]() mutable {
                if (widgetIndex + 1 < sections[index].widgets.size()) {
                  std::swap(sections[index].widgets[widgetIndex], sections[index].widgets[widgetIndex + 1]);
                  commit(std::move(sections));
                }
              },
          }));
          widgetRow->addChild(ui::button({
              .glyph = "close",
              .glyphSize = Style::fontSizeCaption * ctx.scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = "Remove widget from section",
              .minWidth = Style::controlHeightSm * ctx.scale,
              .minHeight = Style::controlHeightSm * ctx.scale,
              .padding = Style::spaceXs * ctx.scale,
              .onClick = [sections = std::vector<BarSectionConfig>(sections), index, widgetIndex, commit,
                          resolvedWidget, owningBar, currentPlacements, setOverrides = ctx.setOverrides,
                          clearOverride = ctx.clearOverride, overridePath,
                          placementPath = std::vector<std::string>{"bar", entry.path[1], "widget_placement"}]() mutable {
                sections[index].widgets.erase(
                    sections[index].widgets.begin() + static_cast<std::ptrdiff_t>(widgetIndex)
                );
                const bool orphan = owningBar != nullptr && !resolvedWidget.second.empty()
                    && barWidgetPlacementReferenceCount(*owningBar, resolvedWidget.second) == 1;
                if (!orphan) {
                  commit(std::move(sections));
                  return;
                }
                auto placements = currentPlacements;
                std::erase_if(placements, [&resolvedWidget](const auto& placement) {
                  return placement.id == resolvedWidget.second;
                });
                setOverrides({{overridePath, sections}, {placementPath, placements}});
                clearOverride({"shell", "material_overrides", "surfaces",
                               noctalia::bar::barWidgetMaterialTarget(resolvedWidget.second)});
              },
          }));
          card->addChild(std::move(widgetRow));
          if (ctx.showAdvanced && !resolvedWidget.second.empty()) {
            const auto placement = std::ranges::find(
                currentPlacements, resolvedWidget.second, &BarWidgetPlacementConfig::id
            );
            if (placement != currentPlacements.end()) {
              const auto colorEditor = [&](bool icon) {
                const auto configured = icon ? placement->iconForeground : placement->foreground;
                return makeGroupColorControl(
                    ctx, configured ? colorSpecToConfigString(*configured) : std::string{}, true,
                    [placements = currentPlacements, placementId = resolvedWidget.second, icon,
                     setOverride = ctx.setOverride, barName = entry.path[1]](std::optional<ColorSpec> color) mutable {
                      const auto row = std::ranges::find(placements, placementId, &BarWidgetPlacementConfig::id);
                      if (row == placements.end()) return;
                      if (icon) row->iconForeground = std::move(color);
                      else row->foreground = std::move(color);
                      setOverride({"bar", barName, "widget_placement"}, std::move(placements));
                    }
                );
              };
              addSectionControl(*card, "Widget text color", "Unset inherits widget/bar text",
                                colorEditor(false), ctx.scale);
              addSectionControl(*card, "Widget icon color", "Unset inherits widget/bar icon color",
                                colorEditor(true), ctx.scale);
            }
          }
        }
        Input* widgetInputPtr = nullptr;
        auto widgetAddRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * ctx.scale, .fillWidth = true});
        widgetAddRow->addChild(ui::input({
            .out = &widgetInputPtr,
            .placeholder = "Widget name or type",
            .fontSize = Style::fontSizeCaption * ctx.scale,
            .controlHeight = Style::controlHeightSm * ctx.scale,
            .horizontalPadding = Style::spaceSm * ctx.scale,
            .flexGrow = 1.0F,
            .onChange = [widgetInputPtr](const std::string&) { widgetInputPtr->setInvalid(false); },
            .onSubmit = [widgetInputPtr, sections = std::vector<BarSectionConfig>(sections), index,
                         appendWidget](const std::string& text) mutable {
              const std::string widget = trimSectionText(text);
              if (widget.empty()) {
                widgetInputPtr->setInvalid(true);
                return;
              }
              appendWidget(std::move(sections), index, widget);
            },
        }));
        widgetAddRow->addChild(ui::button({
            .text = "Add",
            .glyph = "add",
            .fontSize = Style::fontSizeCaption * ctx.scale,
            .glyphSize = Style::fontSizeCaption * ctx.scale,
            .variant = ButtonVariant::Ghost,
            .minHeight = Style::controlHeightSm * ctx.scale,
            .paddingV = Style::spaceXs * ctx.scale,
            .paddingH = Style::spaceSm * ctx.scale,
            .onClick = [widgetInputPtr, sections = std::vector<BarSectionConfig>(sections), index,
                        appendWidget]() mutable {
              const std::string widget = trimSectionText(widgetInputPtr->value());
              if (widget.empty()) {
                widgetInputPtr->setInvalid(true);
                return;
              }
              appendWidget(std::move(sections), index, widget);
            },
        }));
        card->addChild(std::move(widgetAddRow));

        card->addChild(ui::separator());
        card->addChild(makeLabel(
            "Section background actions", Style::fontSizeCaption * ctx.scale,
            colorSpecFromRole(ColorRole::Secondary), FontWeight::Bold
        ));
        card->addChild(makeLabel(
            "These bindings run only where no child widget handles the pointer.", Style::fontSizeMini * ctx.scale,
            colorSpecFromRole(ColorRole::OnSurfaceVariant)
        ));
        for (const auto gesture : noctalia::bar::allGestures()) {
          const std::string key(noctalia::bar::gestureConfigKey(gesture));
          const auto actionIt = current.actionArea.actions.find(key);
          auto actionInput = ui::input({
              .value = actionIt != current.actionArea.actions.end() ? actionIt->second : std::string{},
              .placeholder = "Blank inherits/no action",
              .fontSize = Style::fontSizeCaption * ctx.scale,
              .controlHeight = Style::controlHeightSm * ctx.scale,
              .horizontalPadding = Style::spaceSm * ctx.scale,
              .width = 280.0F * ctx.scale,
              .onSubmit = [sections = std::vector<BarSectionConfig>(sections), index, key, commit](const std::string& text) mutable {
                const std::string action = trimSectionText(text);
                if (action.empty()) sections[index].actionArea.actions.erase(key);
                else sections[index].actionArea.actions.insert_or_assign(key, action);
                commit(std::move(sections));
              },
              .submitOnFocusLoss = true,
          });
          addSectionControl(
              *card, i18n::tr(std::string(noctalia::bar::gestureLabelKey(gesture))), {}, std::move(actionInput), ctx.scale
          );
        }
        block.addChild(std::move(card));
      }

      auto toolbar = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * ctx.scale, .fillWidth = true});
      toolbar->addChild(ui::button({
          .text = "Add section",
          .glyph = "add",
          .fontSize = Style::fontSizeCaption * ctx.scale,
          .glyphSize = Style::fontSizeCaption * ctx.scale,
          .variant = ButtonVariant::Default,
          .minHeight = Style::controlHeightSm * ctx.scale,
          .paddingV = Style::spaceXs * ctx.scale,
          .paddingH = Style::spaceSm * ctx.scale,
          .onClick = [sections = std::vector<BarSectionConfig>(sections), commit]() mutable {
            BarSectionConfig added;
            added.id = nextSectionId(sections);
            added.anchor = BarCenterAlignment::Center;
            added.alignment = BarCenterAlignment::Center;
            sections.push_back(std::move(added));
            commit(std::move(sections));
          },
      }));
      toolbar->addChild(ui::spacer());
      toolbar->addChild(ui::button({
          .text = "Use legacy lanes",
          .fontSize = Style::fontSizeCaption * ctx.scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = "Replace named sections with the start, center, and end lane layout",
          .minHeight = Style::controlHeightSm * ctx.scale,
          .paddingV = Style::spaceXs * ctx.scale,
          .paddingH = Style::spaceSm * ctx.scale,
          .onClick = [commit]() mutable { commit({}); },
      }));
      block.addChild(std::move(toolbar));
    }

  } // namespace

  bool isBarWidgetListPath(const std::vector<std::string>& path) {
    if (path.size() < 3 || path.front() != "bar") {
      return false;
    }
    const auto& key = path.back();
    return key == "start" || key == "center" || key == "end";
  }

  bool isFirstBarWidgetListPath(const std::vector<std::string>& path) {
    return isBarWidgetListPath(path) && path.back() == "start";
  }

  std::string makeLaneSelectionToken(std::string_view laneKey, std::size_t index) {
    return std::string(laneKey) + "#" + std::to_string(index);
  }

  std::optional<LaneSelectionToken> parseLaneSelectionToken(std::string_view token) {
    const auto hash = token.find('#');
    if (hash == std::string_view::npos || hash == 0 || hash + 1 == token.size()) {
      return std::nullopt;
    }
    const std::string_view digits = token.substr(hash + 1);
    std::size_t index = 0;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size()) {
      return std::nullopt;
    }
    return LaneSelectionToken{.laneKey = token.substr(0, hash), .index = index};
  }

  void reindexLaneSelectionAfterRemoval(
      std::vector<std::string>& selection, std::string_view laneKey, std::size_t removedIndex
  ) {
    std::vector<std::string> kept;
    kept.reserve(selection.size());
    for (auto& token : selection) {
      const auto parsed = parseLaneSelectionToken(token);
      if (!parsed.has_value() || parsed->laneKey != laneKey) {
        kept.push_back(std::move(token));
        continue;
      }
      if (parsed->index == removedIndex) {
        continue;
      }
      if (parsed->index > removedIndex) {
        kept.push_back(makeLaneSelectionToken(laneKey, parsed->index - 1));
      } else {
        kept.push_back(std::move(token));
      }
    }
    selection.swap(kept);
  }

  void buildWidgetInspectorBody(
      Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx
  ) {
    addInspectorPane(body, laneListPath, ctx);
  }

  void
  buildCapsuleGroupBody(Flex& body, const std::vector<std::string>& laneListPath, const BarWidgetEditorContext& ctx) {
    addCapsuleGroupInspector(body, laneListPath, ctx);
  }

  void addBarWidgetLaneEditor(Flex& section, const SettingEntry& entry, const BarWidgetEditorContext& ctx) {
    if (!isFirstBarWidgetListPath(entry.path)) {
      return;
    }

    auto block = ui::column(
        {
            .align = FlexAlign::Stretch,
            .gap = Style::spaceSm * ctx.scale,
            .paddingV = 2.0F * ctx.scale,
            .paddingH = 0,
        },
        ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * ctx.scale,
            },
            makeLabel(
                i18n::tr("settings.entities.widget.editor.title"), Style::fontSizeBody * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurface), FontWeight::Normal
            )
        )
    );

    block->addChild(makeSettingSubtitleLabel(i18n::tr("settings.entities.widget.editor.description"), ctx.scale));

    if (ctx.showAdvanced && entry.path.size() >= 2
        && !migrateBarWidgetPlacements(ctx.config, entry.path[1]).empty()) {
      block->addChild(ui::button({
          .text = "Enable per-widget material controls",
          .glyph = "sparkles",
          .fontSize = Style::fontSizeCaption * ctx.scale,
          .glyphSize = Style::fontSizeCaption * ctx.scale,
          .variant = ButtonVariant::Ghost,
          .tooltip = "Save stable instance IDs for this bar; existing module names and order are preserved",
          .minHeight = Style::controlHeightSm * ctx.scale,
          .paddingV = Style::spaceXs * ctx.scale,
          .paddingH = Style::spaceSm * ctx.scale,
          .onClick = [config = &ctx.config, barName = entry.path[1], setOverrides = ctx.setOverrides]() {
            auto batch = migrateBarWidgetPlacements(*config, barName);
            if (!batch.empty()) setOverrides(std::move(batch));
          },
      }));
    }

    bool inheritedSections = false;
    const std::vector<BarSectionConfig> namedSections =
        sectionsForLanePath(ctx.config, entry.path, &inheritedSections);
    if (!namedSections.empty()) {
      addNamedSectionEditor(*block, entry, ctx, namedSections, inheritedSections);
      section.addChild(std::move(block));
      return;
    }

    block->addChild(
        ui::button({
            .text = "Use named sections",
            .glyph = "add",
            .fontSize = Style::fontSizeCaption * ctx.scale,
            .glyphSize = Style::fontSizeCaption * ctx.scale,
            .variant = ButtonVariant::Default,
            .tooltip = "Convert the current start, center, and end lanes into independently positioned sections",
            .minHeight = Style::controlHeightSm * ctx.scale,
            .paddingV = Style::spaceXs * ctx.scale,
            .paddingH = Style::spaceSm * ctx.scale,
            .onClick = [setOverride = ctx.setOverride, path = sectionOverridePath(entry.path), config = &ctx.config,
                        lanePath = entry.path]() {
              setOverride(path, legacySectionsForLanePath(*config, lanePath));
            },
        })
    );

    static constexpr std::string_view kLaneKeys[] = {"start", "center", "end"};

    // Selection toolbar: Group the selected widgets, or clear the current selection.
    if (!ctx.selectedLaneWidgets.empty()) {
      addLaneSelectionToolbar(*block, entry, ctx);
    }

    auto lanes = ui::row({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * ctx.scale,
        .fillWidth = true,
    });

    auto zones = std::make_shared<std::vector<DropZone>>();

    // Shared compact icon-button footprint for lane cards and group headers.
    const float iconSize = Style::controlHeightSm * 0.84F * ctx.scale;
    const float iconPad = 2.0F * ctx.scale;
    const float rowGap = 2.0F * ctx.scale;

    // Wires a drag handle so its card can be dragged between any registered zone (lane or group).
    auto wireDrag = [&ctx, zones, laneListPath = entry.path](
                        Button& handle, Button* handlePtr, Flex* cardPtr, std::size_t homeZoneIndex,
                        std::size_t itemIndex
                    ) {
      auto dragState = std::make_shared<LaneWidgetDragState>();
      handle.setOnPress([dragState, cardPtr, zones, config = &ctx.config, laneListPath,
                         &selectedLaneWidgets = ctx.selectedLaneWidgets, setOverride = ctx.setOverride,
                         setOverrides = ctx.setOverrides, homeZoneIndex,
                         itemIndex](float localX, float localY, bool pressed) {
        const auto clearHighlight = [&]() {
          if (dragState->highlightZoneIndex.has_value() && dragState->highlightItemIndex.has_value()) {
            setCardCombineHighlight(*zones, *dragState->highlightZoneIndex, *dragState->highlightItemIndex, false);
          }
          dragState->highlightZoneIndex = std::nullopt;
          dragState->highlightItemIndex = std::nullopt;
        };
        if (pressed) {
          dragState->active = true;
          dragState->moved = false;
          dragState->startLocalX = localX;
          dragState->startLocalY = localY;
          dragState->lastLocalX = localX;
          dragState->lastLocalY = localY;
          dragState->targetZoneIndex = std::nullopt;
          dragState->targetInsertionIndex = std::nullopt;
          dragState->combineZoneIndex = std::nullopt;
          dragState->combineItemIndex = std::nullopt;
          clearHighlight();
          cardPtr->setOpacity(0.72F);
          hideDropIndicators(*zones);
          return;
        }
        if (!dragState->active) {
          return;
        }
        dragState->active = false;
        cardPtr->setOpacity(1.0F);
        clearHighlight();
        hideDropIndicators(*zones);
        if (!dragState->moved) {
          return;
        }
        // A move or combine renumbers lane positions, so index-keyed selection tokens no longer
        // address the widgets the user picked.
        if (dragState->combineZoneIndex.has_value() && dragState->combineItemIndex.has_value()) {
          selectedLaneWidgets.clear();
          createGroupByCombine(
              *config, *zones, homeZoneIndex, itemIndex, *dragState->combineZoneIndex, *dragState->combineItemIndex,
              setOverrides
          );
          return;
        }
        if (!dragState->targetZoneIndex.has_value() || !dragState->targetInsertionIndex.has_value()) {
          return;
        }
        selectedLaneWidgets.clear();
        performZoneMove(
            *config, laneListPath, *zones, homeZoneIndex, itemIndex, *dragState->targetZoneIndex,
            *dragState->targetInsertionIndex, setOverride, setOverrides
        );
      });
      handle.setOnPointerMotion([dragState, handlePtr, zones, homeZoneIndex, itemIndex,
                                 scale = ctx.scale](float localX, float localY) {
        if (!dragState->active) {
          return;
        }
        dragState->lastLocalX = localX;
        dragState->lastLocalY = localY;
        if (std::hypot(localX - dragState->startLocalX, localY - dragState->startLocalY)
            >= Style::dragStartThreshold * scale) {
          dragState->moved = true;
        }
        if (!dragState->moved) {
          return;
        }
        const auto clearHighlight = [&]() {
          if (dragState->highlightZoneIndex.has_value() && dragState->highlightItemIndex.has_value()) {
            setCardCombineHighlight(*zones, *dragState->highlightZoneIndex, *dragState->highlightItemIndex, false);
          }
          dragState->highlightZoneIndex = std::nullopt;
          dragState->highlightItemIndex = std::nullopt;
        };
        const auto clear = [&]() {
          dragState->targetZoneIndex = std::nullopt;
          dragState->targetInsertionIndex = std::nullopt;
          dragState->combineZoneIndex = std::nullopt;
          dragState->combineItemIndex = std::nullopt;
          clearHighlight();
          hideDropIndicators(*zones);
        };
        float absX = 0.0F;
        float absY = 0.0F;
        Node::absolutePosition(handlePtr, absX, absY);
        const float sceneX = absX + localX;
        const float sceneY = absY + localY;
        const auto targetZone = zoneAtScenePoint(*zones, sceneX, sceneY);
        if (!targetZone.has_value() || *targetZone >= zones->size()) {
          clear();
          return;
        }
        const DropZone& zone = (*zones)[*targetZone];
        if (zone.itemNodes == nullptr || zone.container == nullptr || zone.indicator == nullptr) {
          clear();
          return;
        }

        // Combine: a loose widget dropped onto the middle of another loose widget forms a new group.
        const bool draggedIsLooseWidget = homeZoneIndex < zones->size()
            && !(*zones)[homeZoneIndex].isGroup
            && itemIndex < (*zones)[homeZoneIndex].items.size()
            && !isCapsuleGroupToken((*zones)[homeZoneIndex].items[itemIndex]);
        if (!zone.isGroup && draggedIsLooseWidget) {
          if (const auto hovered = hoveredItemBand(sceneY, *zone.itemNodes); hovered.has_value()) {
            const std::size_t hoveredIdx = hovered->first;
            const bool onMiddle = hovered->second;
            const bool sameItem = *targetZone == homeZoneIndex && hoveredIdx == itemIndex;
            const bool hoveredIsWidget = hoveredIdx < zone.items.size() && !isCapsuleGroupToken(zone.items[hoveredIdx]);
            if (onMiddle && hoveredIsWidget && !sameItem) {
              if (dragState->highlightZoneIndex != *targetZone || dragState->highlightItemIndex != hoveredIdx) {
                clearHighlight();
                setCardCombineHighlight(*zones, *targetZone, hoveredIdx, true);
                dragState->highlightZoneIndex = targetZone;
                dragState->highlightItemIndex = hoveredIdx;
              }
              dragState->combineZoneIndex = targetZone;
              dragState->combineItemIndex = hoveredIdx;
              dragState->targetZoneIndex = std::nullopt;
              dragState->targetInsertionIndex = std::nullopt;
              hideDropIndicators(*zones);
              return;
            }
          }
        }

        // Insertion (between items / into a group).
        clearHighlight();
        dragState->combineZoneIndex = std::nullopt;
        dragState->combineItemIndex = std::nullopt;
        const std::size_t insertion = insertionIndexForSceneY(sceneY, *zone.itemNodes);
        if (insertionWouldNotMove(homeZoneIndex, *targetZone, itemIndex, insertion)) {
          dragState->targetZoneIndex = std::nullopt;
          dragState->targetInsertionIndex = std::nullopt;
          hideDropIndicators(*zones);
          return;
        }
        dragState->targetZoneIndex = targetZone;
        dragState->targetInsertionIndex = insertion;
        hideDropIndicators(*zones);
        updateDropIndicator(*zone.indicator, *zone.container, *zone.itemNodes, insertion, scale);
      });
    };

    // Builds a draggable widget card (used for both loose lane widgets and group members).
    auto makeWidgetCard = [&ctx, &wireDrag, &entry, iconSize, iconPad, rowGap](
                              const std::string& laneEntry, std::size_t homeZoneIndex, std::size_t itemIndex, bool inherited,
                              std::string_view removeGlyph, std::function<void()> removeAction, bool selectable,
                              bool isSelected, std::function<void()> toggleSelect
                          ) -> std::unique_ptr<Flex> {
      const auto resolved = resolveBarWidgetEntry(ctx.config, entry.path, laneEntry);
      const std::string& name = resolved.first;
      const auto info = widgetReferenceInfo(ctx.config, name, false);
      auto card = ui::column({
          .align = FlexAlign::Stretch,
          .paddingV = 3.0F * ctx.scale,
          .paddingH = Style::spaceXs * ctx.scale,
          .fill = colorSpecFromRole(ColorRole::Surface, 0.72F),
          .radius = Style::scaledRadiusSm(ctx.scale),
          .border = isSelected ? colorSpecFromRole(ColorRole::Primary) : clearColorSpec(),
          .borderWidth = Style::borderWidth,
      });
      auto* cardPtr = card.get();

      // Single compact row: [checkbox] [drag] title… [kind glyph] [settings] [remove].
      auto row = ui::row({.align = FlexAlign::Center, .gap = rowGap});
      if (selectable) {
        row->addChild(
            ui::button({
                .glyph = isSelected ? "checkbox" : "square",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = isSelected ? ButtonVariant::Default : ButtonVariant::Ghost,
                .tooltip = isSelected ? i18n::tr("settings.entities.widget.group.deselect")
                                      : i18n::tr("settings.entities.widget.group.select"),
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = std::move(toggleSelect),
            })
        );
      }
      if (!inherited) {
        Button* dragBtnPtr = nullptr;
        auto dragBtn = ui::button({
            .out = &dragBtnPtr,
            .glyph = "menu-2",
            .glyphSize = Style::fontSizeCaption * ctx.scale,
            .variant = ButtonVariant::Ghost,
            .tooltip = i18n::tr("settings.entities.widget.group.drag"),
            .minWidth = iconSize,
            .minHeight = iconSize,
            .padding = iconPad,
            .radius = Style::scaledRadiusSm(ctx.scale),
            .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
        });
        wireDrag(*dragBtn, dragBtnPtr, cardPtr, homeZoneIndex, itemIndex);
        row->addChild(std::move(dragBtn));
      }
      row->addChild(
          makeGlyph(widgetBadgeGlyph(info.kind), Style::fontSizeCaption * ctx.scale, widgetBadgeGlyphColor(info.kind))
      );
      {
        auto titleLabel = makeLabel(
            info.title, Style::fontSizeCaption * ctx.scale, colorSpecFromRole(ColorRole::OnSurface),
            FontWeight::SemiBold
        );
        titleLabel->setMaxLines(1);
        titleLabel->setFlexGrow(1.0F);
        row->addChild(std::move(titleLabel));
      }
      if (!widgetTypeForReference(ctx.config, name).empty()) {
        row->addChild(
            ui::button({
                .glyph = "settings",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.entities.widget.group.settings-widget"),
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = [openWidgetInspector = ctx.openWidgetInspector, laneListPath = entry.path, name]() {
                  if (openWidgetInspector) {
                    openWidgetInspector(laneListPath, name);
                  }
                },
            })
        );
      }
      {
        bool widgetEnabled = true;
        if (auto it = ctx.config.widgets.find(name); it != ctx.config.widgets.end()) {
          widgetEnabled = it->second.getBool("enabled", true);
        }
        row->addChild(
            ui::button({
                .glyph = widgetEnabled ? "eye" : "eye-off",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = widgetEnabled ? i18n::tr("settings.entities.widget.group.hide-widget")
                                         : i18n::tr("settings.entities.widget.group.show-widget"),
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .opacity = widgetEnabled ? 1.0F : 0.38F,
                .onClick = [setOverride = ctx.setOverride, requestRebuild = ctx.requestRebuild, name, widgetEnabled]() {
                  setOverride({"widget", name, "enabled"}, !widgetEnabled);
                  if (requestRebuild) {
                    requestRebuild();
                  }
                },
            })
        );
      }
      if (!inherited && removeAction) {
        const std::string removeTooltip = removeGlyph == "stack-pop"
            ? i18n::tr("settings.entities.widget.group.remove-from-group")
            : i18n::tr("settings.entities.widget.group.remove-widget");
        row->addChild(
            ui::button({
                .glyph = std::string(removeGlyph),
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = removeTooltip,
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = std::move(removeAction),
            })
        );
      }
      card->addChild(std::move(row));
      if (ctx.showAdvanced && !resolved.second.empty()) {
        const std::string target = noctalia::bar::barWidgetMaterialTarget(resolved.second);
        const auto found = ctx.config.shell.materialOverrides.surfaces.find(target);
        SelectSetting material;
        material.options = {
            {.value = "", .label = "Inherit"},
            {.value = "flat", .label = "Flat"},
            {.value = "neumorphic", .label = "Raised plateau"},
            {.value = "liquid_glass", .label = "Optical glass"},
            {.value = "illustrated", .label = "Illustrated"},
        };
        material.selectedValue = found != ctx.config.shell.materialOverrides.surfaces.end() && found->second.primitive
            ? std::string(Style::materialPrimitiveName(*found->second.primitive)) : std::string{};
        material.clearOnEmpty = true;
        card->addChild(ctx.makeSelect(material, {"shell", "material_overrides", "surfaces", target, "primitive"}));
        const BarConfig* bar = entry.path.size() >= 2 ? findBar(ctx.config, entry.path[1]) : nullptr;
        if (bar != nullptr) {
          const auto placement = std::ranges::find(bar->widgetPlacements, resolved.second,
                                                   &BarWidgetPlacementConfig::id);
          if (placement != bar->widgetPlacements.end()) {
            const auto colorEditor = [&](std::string_view label, bool icon) {
              const auto configured = icon ? placement->iconForeground : placement->foreground;
              return makeGroupColorControl(
                  ctx, configured ? colorSpecToConfigString(*configured) : std::string{}, true,
                  [placements = bar->widgetPlacements, placementId = resolved.second, icon,
                   setOverride = ctx.setOverride, barName = entry.path[1]](std::optional<ColorSpec> color) mutable {
                    const auto row = std::ranges::find(placements, placementId, &BarWidgetPlacementConfig::id);
                    if (row == placements.end()) return;
                    if (icon) row->iconForeground = std::move(color);
                    else row->foreground = std::move(color);
                    setOverride({"bar", barName, "widget_placement"}, std::move(placements));
                  }
              );
            };
            addSectionControl(*card, "Text color", "Unset inherits the widget/bar foreground",
                              colorEditor("Text color", false), ctx.scale);
            addSectionControl(*card, "Icon color", "Unset inherits text or the widget/bar icon color",
                              colorEditor("Icon color", true), ctx.scale);
          }
        }
      }
      return card;
    };

    for (const auto laneKey : kLaneKeys) {
      auto lanePath = pathWithLastSegment(entry.path, std::string(laneKey));
      const auto laneItems = barWidgetItemsForPath(ctx.config, lanePath);
      // Lane content includes the styles of the capsule groups it holds, so an edit that only lands
      // in the scope's capsule_group array still marks its lane as overridden.
      const bool overridden = ctx.configService != nullptr && ctx.configService->hasEffectiveBarLaneOverride(lanePath);
      const bool hasGuiOverride = ctx.configService != nullptr && ctx.configService->hasOverride(lanePath);
      const bool monitorLaneExplicit = monitorWidgetListHasExplicitValue(ctx.config, lanePath);
      const bool inherited = isMonitorWidgetListPath(lanePath) && !monitorLaneExplicit;

      auto lane = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * ctx.scale,
          .padding = Style::spaceSm * ctx.scale,
          .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.45F),
          .radius = Style::scaledRadiusMd(ctx.scale),
          .border = colorSpecFromRole(ColorRole::Outline),
          .minWidth = 160.0F * ctx.scale,
          .flexGrow = 1.0F,
      });
      auto* lanePtr = lane.get();

      auto dropIndicator = ui::box({
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = std::max(1.0F, 1.5F * ctx.scale),
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Box& box) { box.setZIndex(10); },
      });
      auto* dropIndicatorPtr = dropIndicator.get();
      lane->addChild(std::move(dropIndicator));

      auto laneItemNodes = std::make_shared<std::vector<Flex*>>();
      laneItemNodes->reserve(laneItems.size());
      const std::size_t laneZoneIndex = zones->size();
      zones->push_back(
          DropZone{
              .isGroup = false,
              .lanePath = lanePath,
              .groupId = {},
              .items = laneItems,
              .container = lanePtr,
              .indicator = dropIndicatorPtr,
              .itemNodes = laneItemNodes,
          }
      );

      auto laneHeader = ui::row(
          {
              .align = FlexAlign::Center,
              .gap = Style::spaceXs * ctx.scale,
              // Fixed height so lanes with an Override badge / Reset button line up with plain ones.
              .minHeight = Style::controlHeightSm * ctx.scale,
          },
          makeLabel(
              laneLabel(laneKey), Style::fontSizeBody * ctx.scale, colorSpecFromRole(ColorRole::OnSurface),
              FontWeight::Bold
          )
      );
      if (overridden) {
        laneHeader->addChild(
            ui::row(
                {
                    .align = FlexAlign::Center,
                    .paddingV = 0,
                    .paddingH = Style::spaceXs * ctx.scale,
                    .fill = colorSpecFromRole(ColorRole::Primary, 0.15F),
                    .radius = Style::scaledRadiusSm(ctx.scale),
                },
                makeLabel(
                    i18n::tr("settings.badges.override"), Style::fontSizeCaption * ctx.scale,
                    colorSpecFromRole(ColorRole::Primary), FontWeight::Bold
                )
            )
        );
      }
      if (inherited) {
        laneHeader->addChild(
            ui::row(
                {
                    .align = FlexAlign::Center,
                    .paddingV = 0,
                    .paddingH = Style::spaceXs * ctx.scale,
                    .fill = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.14F),
                    .radius = Style::scaledRadiusSm(ctx.scale),
                },
                makeLabel(
                    i18n::tr("settings.badges.inherited"), Style::fontSizeCaption * ctx.scale,
                    colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Bold
                )
            )
        );
      }
      laneHeader->addChild(ui::spacer());
      if (inherited) {
        const auto& items = laneItems;
        const auto& path = lanePath;
        laneHeader->addChild(
            ui::button({
                .text = i18n::tr("settings.entities.widget.lanes.customize"),
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .onClick = [setOverride = ctx.setOverride, items, path]() { setOverride(path, items); },
            })
        );
      }
      // Reset reverts the whole lane: its widget list and the capsule groups it holds.
      if (overridden || (monitorLaneExplicit && hasGuiOverride)) {
        laneHeader->addChild(ctx.makeResetActionButton(
            lanePath, [&selectedLaneWidgets = ctx.selectedLaneWidgets, resetBarLane = ctx.resetBarLane, lanePath]() {
              // Lane contents are replaced wholesale; every index-keyed token in it is stale.
              selectedLaneWidgets.clear();
              resetBarLane(lanePath);
            }
        ));
      }
      lane->addChild(std::move(laneHeader));

      const std::vector<BarCapsuleGroupStyle> laneGroups = capsuleGroupsForLanePath(ctx.config, lanePath);
      for (std::size_t i = 0; i < laneItems.size(); ++i) {
        const std::string& entryName = laneItems[i];

        // Group token → render a container holding its members; members are dragged in/out of it.
        if (isCapsuleGroupToken(entryName)) {
          const std::string gid = capsuleGroupTokenId(entryName);
          const BarCapsuleGroupStyle* group = findCapsuleGroupStyle(laneGroups, gid);
          if (group == nullptr) {
            auto orphan = ui::column({
                .align = FlexAlign::Center,
                .gap = Style::spaceXs * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .border = colorSpecFromRole(ColorRole::Error, 0.5F),
            });
            orphan->addChild(makeLabel(
                i18n::tr("settings.entities.widget.group.orphan"), Style::fontSizeCaption * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
            ));
            if (!inherited) {
              orphan->addChild(
                  ui::button({
                      .glyph = "close",
                      .glyphSize = Style::fontSizeCaption * ctx.scale,
                      .variant = ButtonVariant::Ghost,
                      .tooltip = i18n::tr("settings.entities.widget.group.remove-orphan"),
                      .minHeight = Style::controlHeightSm * ctx.scale,
                      .padding = Style::spaceXs * ctx.scale,
                      .radius = Style::scaledRadiusSm(ctx.scale),
                      .onClick = [&selectedLaneWidgets = ctx.selectedLaneWidgets, setOverride = ctx.setOverride,
                                  items = laneItems, lanePath, laneKey, i]() mutable {
                        items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                        reindexLaneSelectionAfterRemoval(selectedLaneWidgets, laneKey, i);
                        setOverride(lanePath, items);
                      },
                  })
              );
            }
            laneItemNodes->push_back(orphan.get());
            lane->addChild(std::move(orphan));
            continue;
          }

          // Tint the container by the group's own fill so groups with different colors are distinguishable.
          // A color meant to blend into surfaces (e.g. surface_variant) can't separate the box from the lane,
          // so fall back to a neutral border + slight surface lift when the fill is too close to the lane.
          const Color groupFillColor = resolveColorSpec(group->fill);
          const Color laneBgColor = colorForRole(ColorRole::SurfaceVariant);
          const float dr = groupFillColor.r - laneBgColor.r;
          const float dg = groupFillColor.g - laneBgColor.g;
          const float db = groupFillColor.b - laneBgColor.b;
          const bool fillDistinct = std::sqrt(dr * dr + dg * dg + db * db) >= 0.15F;
          ColorSpec groupFillTint;
          ColorSpec groupBorder;
          if (fillDistinct) {
            groupFillTint = group->fill;
            groupFillTint.alpha *= 0.15F;
            groupBorder = group->fill; // full opacity
          } else {
            groupFillTint = colorSpecFromRole(ColorRole::OnSurface, 0.06F); // slight neutral lift
            groupBorder = colorSpecFromRole(ColorRole::Outline);            // full opacity
          }
          auto container = ui::column({
              .align = FlexAlign::Stretch,
              .gap = Style::spaceXs * ctx.scale,
              .padding = Style::spaceXs * ctx.scale,
              .fill = groupFillTint,
              .radius = Style::scaledRadiusSm(ctx.scale),
              .border = groupBorder,
              .opacity = group->enabled ? 1.0F : 0.45F,
          });
          auto* containerPtr = container.get();

          auto groupIndicator = ui::box({
              .fill = colorSpecFromRole(ColorRole::Primary),
              .radius = std::max(1.0F, 1.5F * ctx.scale),
              .visible = false,
              .participatesInLayout = false,
              .configure = [](Box& box) { box.setZIndex(10); },
          });
          auto* groupIndicatorPtr = groupIndicator.get();
          container->addChild(std::move(groupIndicator));

          auto groupHeader = ui::row({.align = FlexAlign::Center, .gap = rowGap});
          groupHeader->addChild(
              ui::box({
                  .fill = group->fill,
                  .radius = std::max(1.0F, 2.0F * ctx.scale),
                  .width = Style::fontSizeCaption * ctx.scale,
                  .height = Style::fontSizeCaption * ctx.scale,
                  .configure = [](Box& box) {
                    box.setBorder(colorSpecFromRole(ColorRole::Outline), Style::borderWidth);
                  },
              })
          );
          {
            auto groupLabel = makeLabel(
                i18n::tr("settings.entities.widget.group.title"), Style::fontSizeCaption * ctx.scale,
                colorSpecFromRole(ColorRole::OnSurface), FontWeight::SemiBold
            );
            groupLabel->setFlexGrow(1.0F);
            groupHeader->addChild(std::move(groupLabel));
          }
          groupHeader->addChild(
              ui::button({
                  .glyph = group->enabled ? "eye" : "eye-off",
                  .glyphSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .tooltip = group->enabled ? i18n::tr("settings.entities.widget.group.hide")
                                            : i18n::tr("settings.entities.widget.group.show"),
                  .minWidth = iconSize,
                  .minHeight = iconSize,
                  .padding = iconPad,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .opacity = group->enabled ? 1.0F : 0.38F,
                  .onClick = [setOverrides = ctx.setOverrides, groups = laneGroups, lanePathCopy = lanePath, gid,
                              requestRebuild = ctx.requestRebuild]() {
                    std::vector<BarCapsuleGroupStyle> updated = groups;
                    for (auto& g : updated) {
                      if (g.id == gid) {
                        g.enabled = !g.enabled;
                        break;
                      }
                    }
                    const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(lanePathCopy);
                    if (!groupPath.empty()) {
                      setOverrides({{groupPath, updated}});
                      if (requestRebuild) {
                        requestRebuild();
                      }
                    }
                  },
              })
          );
          groupHeader->addChild(
              ui::button({
                  .glyph = "settings",
                  .glyphSize = Style::fontSizeCaption * ctx.scale,
                  .variant = ButtonVariant::Ghost,
                  .tooltip = i18n::tr("settings.entities.widget.group.edit"),
                  .minWidth = iconSize,
                  .minHeight = iconSize,
                  .padding = iconPad,
                  .radius = Style::scaledRadiusSm(ctx.scale),
                  .onClick = [openCapsuleGroupInspector = ctx.openCapsuleGroupInspector, laneListPath = entry.path,
                              gid]() {
                    if (openCapsuleGroupInspector) {
                      openCapsuleGroupInspector(laneListPath, gid);
                    }
                  },
              })
          );
          if (!inherited) {
            groupHeader->addChild(
                ui::button({
                    .glyph = "stack-pop",
                    .glyphSize = Style::fontSizeCaption * ctx.scale,
                    .variant = ButtonVariant::Ghost,
                    .tooltip = i18n::tr("settings.entities.widget.group.ungroup"),
                    .minWidth = iconSize,
                    .minHeight = iconSize,
                    .padding = iconPad,
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .onClick = [&selectedLaneWidgets = ctx.selectedLaneWidgets, config = &ctx.config, lanePath, gid,
                                setOverrides = ctx.setOverrides]() {
                      std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(*config, lanePath);
                      const BarCapsuleGroupStyle* g = findCapsuleGroupStyle(groups, gid);
                      if (g == nullptr) {
                        return;
                      }
                      const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(lanePath);
                      if (groupPath.empty()) {
                        return;
                      }
                      const std::vector<std::string> members = g->members;
                      std::vector<std::string> laneEntries = barWidgetItemsForPath(*config, lanePath);
                      const std::string token = makeCapsuleGroupToken(gid);
                      const auto it = std::ranges::find(laneEntries, token);
                      if (it != laneEntries.end()) {
                        const std::size_t pos = static_cast<std::size_t>(it - laneEntries.begin());
                        laneEntries.erase(laneEntries.begin() + static_cast<std::ptrdiff_t>(pos));
                        laneEntries.insert(
                            laneEntries.begin() + static_cast<std::ptrdiff_t>(pos), members.begin(), members.end()
                        );
                      }
                      std::vector<BarCapsuleGroupStyle> remaining;
                      for (const auto& x : groups) {
                        if (x.id != gid) {
                          remaining.push_back(x);
                        }
                      }
                      selectedLaneWidgets.clear();
                      setOverrides({{lanePath, laneEntries}, {groupPath, remaining}});
                    },
                })
            );
            Button* groupDragPtr = nullptr;
            auto groupDrag = ui::button({
                .out = &groupDragPtr,
                .glyph = "menu-2",
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.entities.widget.group.drag"),
                .minWidth = iconSize,
                .minHeight = iconSize,
                .padding = iconPad,
                .radius = Style::scaledRadiusSm(ctx.scale),
                .configure = [](Button& button) { button.setCursorShape(WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE); },
            });
            wireDrag(*groupDrag, groupDragPtr, containerPtr, laneZoneIndex, i);
            groupHeader->addChild(std::move(groupDrag));
          }
          container->addChild(std::move(groupHeader));

          auto groupItemNodes = std::make_shared<std::vector<Flex*>>();
          const std::size_t groupZoneIndex = zones->size();
          zones->push_back(
              DropZone{
                  .isGroup = true,
                  .lanePath = {},
                  .groupId = gid,
                  .items = group->members,
                  .container = containerPtr,
                  .indicator = groupIndicatorPtr,
                  .itemNodes = groupItemNodes,
              }
          );

          for (std::size_t m = 0; m < group->members.size(); ++m) {
            std::function<void()> eject;
            if (!inherited) {
              eject = [&selectedLaneWidgets = ctx.selectedLaneWidgets, config = &ctx.config, lanePath, gid, m,
                       setOverrides = ctx.setOverrides]() {
                const std::vector<std::string> groupPath = capsuleGroupPathForLanePath(lanePath);
                if (groupPath.empty()) {
                  return;
                }
                std::vector<BarCapsuleGroupStyle> groups = capsuleGroupsForLanePath(*config, lanePath);
                std::string ejected;
                bool emptyNow = false;
                for (auto& g : groups) {
                  if (g.id == gid) {
                    if (m < g.members.size()) {
                      ejected = g.members[m];
                      g.members.erase(g.members.begin() + static_cast<std::ptrdiff_t>(m));
                    }
                    emptyNow = g.members.empty();
                    break;
                  }
                }
                if (ejected.empty()) {
                  return;
                }
                std::vector<std::string> laneEntries = barWidgetItemsForPath(*config, lanePath);
                const std::string token = makeCapsuleGroupToken(gid);
                const auto it = std::ranges::find(laneEntries, token);
                std::size_t insertAt = it != laneEntries.end() ? static_cast<std::size_t>(it - laneEntries.begin()) + 1
                                                               : laneEntries.size();
                if (emptyNow && it != laneEntries.end()) {
                  const std::size_t pos = static_cast<std::size_t>(it - laneEntries.begin());
                  laneEntries.erase(laneEntries.begin() + static_cast<std::ptrdiff_t>(pos));
                  insertAt = pos;
                  std::vector<BarCapsuleGroupStyle> kept;
                  for (const auto& x : groups) {
                    if (x.id != gid) {
                      kept.push_back(x);
                    }
                  }
                  groups.swap(kept);
                }
                insertAt = std::min(insertAt, laneEntries.size());
                laneEntries.insert(laneEntries.begin() + static_cast<std::ptrdiff_t>(insertAt), ejected);
                selectedLaneWidgets.clear();
                setOverrides({{lanePath, laneEntries}, {groupPath, groups}});
              };
            }
            auto memberCard = makeWidgetCard(
                group->members[m], groupZoneIndex, m, inherited, "stack-pop", std::move(eject), false, false,
                std::function<void()>{}
            );
            groupItemNodes->push_back(memberCard.get());
            container->addChild(std::move(memberCard));
          }

          laneItemNodes->push_back(containerPtr);
          lane->addChild(std::move(container));
          continue;
        }

        // Loose widget card.
        const std::string selectionToken = makeLaneSelectionToken(laneKey, i);
        const bool isSelected = std::ranges::contains(ctx.selectedLaneWidgets, selectionToken);
        std::function<void()> removeClose;
        if (!inherited) {
          auto items = laneItems;
          items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
          const auto resolvedEntry = resolveBarWidgetEntry(ctx.config, lanePath, entryName);
          const std::string resolvedEntryName = resolvedEntry.first;
          const bool removeInstance = isGuiManagedNamedWidgetInstance(ctx, resolvedEntryName)
              && !widgetHasPlacementAfterLaneEdit(ctx.config, lanePath, items, resolvedEntryName);
          const BarConfig* owningBar = lanePath.size() >= 2 ? findBar(ctx.config, lanePath[1]) : nullptr;
          const bool removePlacement = owningBar != nullptr && !resolvedEntry.second.empty()
              && barWidgetPlacementReferenceCount(*owningBar, resolvedEntry.second) == 1;
          auto placements = owningBar != nullptr ? owningBar->widgetPlacements : std::vector<BarWidgetPlacementConfig>{};
          if (removePlacement) std::erase_if(placements, [&resolvedEntry](const auto& placement) {
            return placement.id == resolvedEntry.second;
          });
          removeClose = [&selectedLaneWidgets = ctx.selectedLaneWidgets, setOverride = ctx.setOverride,
                         setOverrides = ctx.setOverrides, clearOverride = ctx.clearOverride,
                         items = std::move(items), lanePath, placements = std::move(placements),
                         placementId = resolvedEntry.second, removePlacement,
                         entryName = resolvedEntryName,
                         removeInstance, laneKey, i]() {
            reindexLaneSelectionAfterRemoval(selectedLaneWidgets, laneKey, i);
            if (removePlacement) {
              setOverrides({
                  {lanePath, items},
                  {{"bar", lanePath[1], "widget_placement"}, placements},
              });
              clearOverride({"shell", "material_overrides", "surfaces",
                             noctalia::bar::barWidgetMaterialTarget(placementId)});
            } else {
              setOverride(lanePath, items);
            }
            if (removeInstance) {
              clearOverride({"widget", entryName});
            }
          };
        }
        std::function<void()> toggleSelect;
        if (!inherited) {
          toggleSelect = [&selectedLaneWidgets = ctx.selectedLaneWidgets, selectionToken,
                          requestRebuild = ctx.requestRebuild]() {
            const auto it = std::ranges::find(selectedLaneWidgets, selectionToken);
            if (it != selectedLaneWidgets.end()) {
              selectedLaneWidgets.erase(it);
            } else {
              selectedLaneWidgets.push_back(selectionToken);
            }
            requestRebuild();
          };
        }
        auto card = makeWidgetCard(
            entryName, laneZoneIndex, i, inherited, "close", std::move(removeClose), !inherited, isSelected,
            std::move(toggleSelect)
        );
        laneItemNodes->push_back(card.get());
        lane->addChild(std::move(card));
      }

      if (laneItems.empty() && !inherited) {
        lane->addChild(
            ui::column(
                {
                    .align = FlexAlign::Center,
                    .gap = 2.0F * ctx.scale,
                    .paddingV = Style::spaceMd * ctx.scale,
                    .paddingH = Style::spaceSm * ctx.scale,
                    .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.25F),
                    .radius = Style::scaledRadiusSm(ctx.scale),
                    .border = colorSpecFromRole(ColorRole::Outline),
                },
                makeLabel(
                    i18n::tr("settings.entities.widget.lanes.empty"), Style::fontSizeCaption * ctx.scale,
                    colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Bold
                ),
                makeLabel(
                    i18n::tr("settings.entities.widget.lanes.empty-hint"), Style::fontSizeCaption * ctx.scale,
                    colorSpecFromRole(ColorRole::OnSurfaceVariant), FontWeight::Normal
                )
            )
        );
      }

      if (!inherited) {
        lane->addChild(
            ui::button({
                .text = i18n::tr("settings.entities.widget.add"),
                .glyph = "add",
                .fontSize = Style::fontSizeCaption * ctx.scale,
                .glyphSize = Style::fontSizeCaption * ctx.scale,
                .variant = ButtonVariant::Ghost,
                .minHeight = Style::controlHeightSm * ctx.scale,
                .paddingV = Style::spaceXs * ctx.scale,
                .paddingH = Style::spaceSm * ctx.scale,
                .radius = Style::scaledRadiusMd(ctx.scale),
                .onClick = [&editingWidgetName = ctx.editingWidgetName, &renamingWidgetName = ctx.renamingWidgetName,
                            &pendingDeleteWidgetName = ctx.pendingDeleteWidgetName,
                            &pendingDeleteWidgetSettingPath = ctx.pendingDeleteWidgetSettingPath,
                            openWidgetAddPopup = ctx.openWidgetAddPopup, lanePath]() {
                  editingWidgetName.clear();
                  renamingWidgetName.clear();
                  pendingDeleteWidgetName.clear();
                  pendingDeleteWidgetSettingPath.clear();
                  if (openWidgetAddPopup) {
                    openWidgetAddPopup(lanePath);
                  }
                },
            })
        );
      }

      lanes->addChild(std::move(lane));
    }

    block->addChild(std::move(lanes));
    section.addChild(std::move(block));
  }

} // namespace settings
