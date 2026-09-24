#include "shell/settings/settings_content_plugins.h"

#include "config/config_types.h"
#include "config/config_service.h"
#include "i18n/i18n.h"
#include "net/url_open.h"
#include "render/scene/input_area.h"
#include "scripting/plugin_api.h"
#include "scripting/plugin_i18n.h"
#include "scripting/plugin_id.h"
#include "scripting/plugin_panel_shell.h"
#include "scripting/plugin_registry.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_control_factory.h"
#include "shell/settings/settings_registry.h"
#include "shell/settings/style_gallery.h"
#include "shell/settings/widget_settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/dialogs/glyph_picker_dialog.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace settings {

  namespace {
    std::unique_ptr<Label>
    makeLabel(std::string_view text, float fontSize, ColorRole role, FontWeight weight = FontWeight::Normal) {
      return ui::label({
          .text = std::string(text),
          .fontSize = fontSize,
          .fontWeight = weight,
          .color = colorSpecFromRole(role),
      });
    }

    std::unique_ptr<Button> makeConfirmButton(
        std::string text, ButtonVariant variant, float scale, std::function<void()> onClick, std::string glyph = {}
    ) {
      ui::ButtonProps props;
      props.text = std::move(text);
      if (!glyph.empty()) {
        props.glyph = std::move(glyph);
        props.glyphSize = Style::fontSizeBody * scale;
      }
      props.fontSize = Style::fontSizeCaption * scale;
      props.variant = variant;
      props.minHeight = Style::controlHeightSm * scale;
      props.paddingV = Style::spaceXs * scale;
      props.paddingH = Style::spaceSm * scale;
      props.radius = Style::scaledRadiusSm(scale);
      props.onClick = std::move(onClick);
      return ui::button(std::move(props));
    }

    std::unique_ptr<Flex> galleryBar(const StyleGalleryPreview& preview, bool vertical, float scale) {
      const bool segmented = preview.barStyle == "islands";
      auto bar = ui::row({
          .align = FlexAlign::Stretch,
          .gap = segmented ? 3.0F * scale : 0.0F,
          .fill = colorSpecFromRole(ColorRole::Primary, 0.82F),
          .radius = (preview.barStyle == "full" || preview.barStyle == "notch") ? 0.0F : 6.0F * scale,
          .width = vertical ? 13.0F * scale : std::optional<float>{},
          .height = vertical ? std::optional<float>{} : 13.0F * scale,
          .flexGrow = vertical || preview.barStyle == "full" ? 1.0F : std::optional<float>{},
      });
      if (segmented) {
        bar->clearFill();
        for (int i = 0; i < 3; ++i)
          bar->addChild(ui::box({.fill = colorSpecFromRole(ColorRole::Primary, 0.82F), .radius = 5.0F * scale,
                                .width = vertical ? 13.0F * scale : std::optional<float>{},
                                .height = vertical ? std::optional<float>{} : 13.0F * scale,
                                .flexGrow = 1.0F}));
      }
      return bar;
    }

    std::unique_ptr<Flex> galleryWorkspace(const StyleGalleryPreview& preview, float scale) {
      auto workspace = ui::row({.align = FlexAlign::Stretch, .gap = 5.0F * scale, .padding = 7.0F * scale, .flexGrow = 1.0F});
      const float radius = preview.radius == 0.0F ? 0.0F : std::clamp(preview.radius * 0.18F, 1.0F, 12.0F) * scale;
      const float border = preview.border > 0.0F ? std::max(1.0F, preview.border * 0.35F) * scale : 0.0F;
      const auto window = [&](float grow) {
        return ui::box({
            .fill = colorSpecFromRole(ColorRole::Surface, preview.material == "glass" ? 0.52F : 0.90F),
            .border = colorSpecFromRole(ColorRole::Outline, 0.72F),
            .borderWidth = border,
            .radius = radius,
            .flexGrow = grow,
        });
      };
      if (preview.layout == "tiled") {
        workspace->addChild(window(1.5F));
        auto stack = ui::column({.align = FlexAlign::Stretch, .gap = 5.0F * scale, .flexGrow = 1.0F});
        stack->addChild(window(1.0F));
        stack->addChild(window(1.0F));
        workspace->addChild(std::move(stack));
      } else if (preview.layout == "floating") {
        workspace->setPadding(12.0F * scale, 18.0F * scale);
        workspace->addChild(window(1.0F));
      } else {
        workspace->addChild(window(1.0F));
        workspace->addChild(window(1.0F));
        workspace->addChild(window(0.72F));
      }
      return workspace;
    }

    std::unique_ptr<Flex> gallerySchematic(const StyleGalleryPreview& preview, float scale) {
      const bool vertical = preview.position == "left" || preview.position == "right";
      auto desktop = vertical
          ? ui::row({.align = FlexAlign::Stretch, .gap = 5.0F * scale, .padding = 5.0F * scale,
                     .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.78F),
                     .radius = 9.0F * scale, .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
                     .borderWidth = Style::borderWidth, .fillWidth = true, .clipChildren = true,
                     .height = 105.0F * scale})
          : ui::column({.align = FlexAlign::Stretch, .gap = 5.0F * scale, .padding = 5.0F * scale,
                        .fill = colorSpecFromRole(ColorRole::SurfaceVariant, 0.78F),
                        .radius = 9.0F * scale, .border = colorSpecFromRole(ColorRole::Outline, 0.45F),
                        .borderWidth = Style::borderWidth, .fillWidth = true, .clipChildren = true,
                        .height = 105.0F * scale});
      auto bar = galleryBar(preview, vertical, scale);
      auto workspace = galleryWorkspace(preview, scale);
      if (preview.position == "bottom" || preview.position == "right") {
        desktop->addChild(std::move(workspace));
        desktop->addChild(std::move(bar));
      } else {
        desktop->addChild(std::move(bar));
        desktop->addChild(std::move(workspace));
      }
      return desktop;
    }

    std::unique_ptr<Button> galleryCard(
        const StyleGalleryEntry& entry, std::string_view current, bool enabled, float scale,
        std::function<void()> onClick
    ) {
      auto card = ui::button({
          .enabled = enabled,
          .selected = entry.name == current,
          .contentAlign = ButtonContentAlign::Start,
          .variant = entry.name == current ? ButtonVariant::Primary : ButtonVariant::Outline,
          .badge = entry.category == "factory" ? i18n::tr("settings.plugins.style-gallery.badge-built-in")
              : entry.category == "reference" ? i18n::tr("settings.plugins.style-gallery.badge-reference")
              : i18n::tr("settings.plugins.style-gallery.badge-saved"),
          .minWidth = 210.0F * scale,
          .maxWidth = 270.0F * scale,
          .padding = Style::spaceSm * scale,
          .gap = Style::spaceXs * scale,
          .radius = Style::scaledRadiusMd(scale),
          .flexGrow = 1.0F,
          .onClick = std::move(onClick),
          .configure = [](Button& button) { button.setDirection(FlexDirection::Vertical); button.setAlign(FlexAlign::Stretch); },
      });
      card->addChild(gallerySchematic(entry.preview, scale));
      card->addChild(makeLabel(entry.name, Style::fontSizeBody * scale, ColorRole::OnSurface, FontWeight::Bold));
      auto description = makeLabel(entry.description, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant);
      description->setMaxLines(2);
      description->setEllipsize(TextEllipsize::End);
      card->addChild(std::move(description));
      const std::string details = entry.preview.material + " · " + entry.preview.layout + " · " +
          i18n::tr(entry.preview.motion ? "settings.plugins.style-gallery.motion" : "settings.plugins.style-gallery.still");
      card->addChild(makeLabel(details, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant));
      return card;
    }

    std::unique_ptr<Flex>
    pluginDeleteConfirmPanel(const scripting::PluginStatus& plugin, const SettingsPluginsContext& ctx, float scale) {
      auto panel = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .padding = Style::spaceSm * scale,
          .configure = [scale](Flex& p) {
            p.setRadius(Style::scaledRadiusSm(scale));
            p.setFill(colorSpecFromRole(ColorRole::Error, 0.10F));
            p.setBorder(colorSpecFromRole(ColorRole::Error, 0.5F), Style::borderWidth);
          },
      });
      panel->addChild(makeLabel(
          i18n::tr("settings.plugins.plugins.delete-confirm-title", "name", plugin.name), Style::fontSizeBody * scale,
          ColorRole::Error, FontWeight::Bold
      ));
      panel->addChild(makeLabel(
          i18n::tr("settings.plugins.plugins.delete-confirm-desc"), Style::fontSizeCaption * scale,
          ColorRole::OnSurfaceVariant
      ));
      panel->addChild(
          ui::row(
              {.align = FlexAlign::Center, .gap = Style::spaceSm * scale}, ui::spacer(),
              makeConfirmButton(
                  i18n::tr("common.actions.cancel"), ButtonVariant::Ghost, scale,
                  [cb = ctx.cancelDelete]() {
                    if (cb) {
                      cb();
                    }
                  }
              ),
              makeConfirmButton(
                  i18n::tr("settings.plugins.plugins.delete"), ButtonVariant::Destructive, scale,
                  [cb = ctx.onRemove, id = plugin.id]() {
                    if (cb) {
                      cb(id);
                    }
                  },
                  "trash"
              )
          )
      );
      return panel;
    }

    bool pluginEnabled(const scripting::PluginStatus& plugin, const SettingsPluginsContext& ctx) {
      if (ctx.config == nullptr) {
        return plugin.enabled;
      }
      return std::ranges::contains(ctx.config->plugins.enabled, plugin.id);
    }

    std::string_view pluginDisplayName(const scripting::PluginStatus& plugin) { return plugin.name; }

    std::string pluginSourceDisplayName(std::string_view source) {
      if (source == "official") {
        return "Official";
      }
      if (source == "community") {
        return "Community";
      }
      return std::string(source);
    }

    int pluginSourceOrder(std::string_view source) {
      if (source == "official") {
        return 0;
      }
      if (source == "community") {
        return 1;
      }
      return 2;
    }

    bool pluginSourceLess(std::string_view a, std::string_view b) {
      const int aOrder = pluginSourceOrder(a);
      const int bOrder = pluginSourceOrder(b);
      if (aOrder != bOrder) {
        return aOrder < bOrder;
      }
      return a < b;
    }

    std::unique_ptr<Flex> sourceRow(const PluginSourceConfig& source, const SettingsPluginsContext& ctx, float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      Flex* r = row.get();

      auto info = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
      info->addChild(makeLabel(
          pluginSourceDisplayName(source.name), Style::fontSizeBody * scale,
          source.enabled ? ColorRole::OnSurface : ColorRole::OnSurfaceVariant, FontWeight::Medium
      ));
      const std::string kind = source.kind == PluginSourceKind::Git ? i18n::tr("settings.plugins.sources.kind.git")
                                                                    : i18n::tr("settings.plugins.sources.kind.path");
      info->addChild(
          makeLabel(kind + " · " + source.location, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant)
      );
      r->addChild(std::move(info));

      if (source.enabled && source.kind == PluginSourceKind::Git) {
        r->addChild(
            ui::button({
                .glyph = "refresh",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.plugins.sources.update"),
                .onClick = [cb = ctx.updateSource, name = source.name]() {
                  if (cb) {
                    cb(name);
                  }
                },
            })
        );
      }
      r->addChild(
          ui::button({
              .glyph = "settings",
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Ghost,
              .tooltip = i18n::tr("settings.plugins.sources.edit"),
              .onClick = [cb = ctx.editSource, source]() {
                if (cb) {
                  cb(source);
                }
              },
          })
      );
      r->addChild(
          ui::toggle({
              .checked = source.enabled,
              .scale = scale,
              .onChange = [cb = ctx.setSourceEnabled, source](bool on) {
                if (cb) {
                  cb(source, on);
                }
              },
          })
      );
      return row;
    }

    std::unique_ptr<Flex> makeRoleBadge(std::string_view label, ColorRole role, float scale, float fillAlpha = 0.15F) {
      return ui::row(
          {.align = FlexAlign::Center,
           .paddingH = Style::spaceXs * scale,
           .fill = colorSpecFromRole(role, fillAlpha),
           .radius = Style::scaledRadiusSm(scale)},
          ui::label({
              .text = std::string(label),
              .fontSize = Style::fontSizeCaption * scale,
              .fontWeight = FontWeight::Bold,
              .color = colorSpecFromRole(role),
          })
      );
    }

    std::unique_ptr<Flex>
    pluginRow(const scripting::PluginStatus& plugin, const SettingsPluginsContext& ctx, float scale) {
      auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      Flex* r = row.get();
      const bool enabled = pluginEnabled(plugin, ctx);

      r->addChild(
          ui::glyph({
              .glyph = plugin.icon.empty() ? std::string("apps") : plugin.icon,
              .glyphSize = Style::fontSizeHeader * scale,
              .color = colorSpecFromRole(ColorRole::Primary),
              .width = Style::controlHeightSm * scale,
              .height = Style::controlHeightSm * scale,
          })
      );

      auto info = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
      auto title = ui::row({.align = FlexAlign::Center, .gap = Style::spaceXs * scale});
      const std::string version = plugin.version.empty() ? std::string("?") : plugin.version;
      title->addChild(
          makeLabel(pluginDisplayName(plugin), Style::fontSizeBody * scale, ColorRole::OnSurface, FontWeight::Medium)
      );
      if (plugin.source == "official") {
        title->addChild(makeRoleBadge(i18n::tr("settings.badges.official"), ColorRole::Primary, scale));
      } else if (plugin.source == "community") {
        title->addChild(makeRoleBadge(i18n::tr("settings.badges.community"), ColorRole::Secondary, scale));
      } else if (!plugin.source.empty()) {
        title->addChild(makeRoleBadge(pluginSourceDisplayName(plugin.source), ColorRole::Tertiary, scale));
      }
      title->addChild(makeLabel("v" + version, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant));
      if (!plugin.compatible) {
        title->addChild(makeLabel(
            i18n::tr("settings.plugins.plugins.incompatible-plugin-api"), Style::fontSizeMini * scale, ColorRole::Error,
            FontWeight::Bold
        ));
      }
      if (plugin.heldBack) {
        title->addChild(makeRoleBadge(i18n::tr("settings.plugins.plugins.held-back"), ColorRole::Tertiary, scale));
      }
      if (plugin.deprecated) {
        title->addChild(makeLabel(
            i18n::tr("settings.plugins.plugins.deprecated"), Style::fontSizeMini * scale, ColorRole::Secondary,
            FontWeight::Bold
        ));
      }
      if (plugin.updateAvailable) {
        const std::string badge = plugin.availableVersion.empty()
            ? i18n::tr("settings.plugins.plugins.update-available")
            : i18n::tr("settings.plugins.plugins.update-to", "version", plugin.availableVersion);
        title->addChild(makeRoleBadge(badge, ColorRole::Tertiary, scale));
      }
      info->addChild(std::move(title));
      if (plugin.heldBack) {
        info->addChild(makeLabel(
            i18n::tr(
                "settings.plugins.plugins.held-back-hint", "version", plugin.latestVersion, "required",
                plugin.latestPluginApiVersion, "current", scripting::kCurrentPluginApiVersion
            ),
            Style::fontSizeMini * scale, ColorRole::Tertiary
        ));
      }
      if (!plugin.description.empty()) {
        info->addChild(makeLabel(plugin.description, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant));
      }
      if (!plugin.dependencies.empty()) {
        info->addChild(makeLabel(
            i18n::tr("settings.plugins.plugins.requires", "dependencies", StringUtils::join(plugin.dependencies, ", ")),
            Style::fontSizeCaption * scale, ColorRole::Secondary
        ));
      }
      r->addChild(std::move(info));

      if (const auto pageUrl = scripting::pluginWebsitePageUrl(plugin.source, plugin.id)) {
        r->addChild(
            ui::button({
                .glyph = "external-link",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.plugins.store.open-page"),
                .onClick = [url = *pageUrl]() { (void)net::openInBrowser(url); },
            })
        );
      }

      const auto* manifest = scripting::PluginRegistry::instance().findManifest(plugin.id);
      if (enabled && manifest != nullptr && pluginHasSettings(*manifest) && ctx.onConfigure) {
        r->addChild(
            ui::button({
                .glyph = "settings",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.plugins.plugins.configure"),
                .onClick = [cb = ctx.onConfigure, id = plugin.id]() {
                  if (cb) {
                    cb(id);
                  }
                },
            })
        );
      }

      const bool removable = ctx.onRemove
          && plugin.source != "local"
          && !std::ranges::any_of(ctx.sources, [&](const PluginSourceConfig& s) {
                               return s.name == plugin.source && s.kind == PluginSourceKind::Path;
                             });
      if (removable) {
        r->addChild(
            ui::button({
                .glyph = "trash",
                .glyphSize = Style::fontSizeBody * scale,
                .variant = ButtonVariant::Ghost,
                .tooltip = i18n::tr("settings.plugins.plugins.remove"),
                .onClick = [cb = ctx.requestDeleteConfirm, id = plugin.id]() {
                  if (cb) {
                    cb(id);
                  }
                },
            })
        );
      }

      const bool busy = ctx.isEnabling && ctx.isEnabling(plugin.id);
      if (busy) {
        r->addChild(
            ui::spinner({
                .spinnerSize = Style::controlHeightSm * scale * 0.7F,
                .spinning = true,
            })
        );
      } else {
        r->addChild(
            ui::toggle({
                .checked = enabled,
                .enabled = enabled || plugin.compatible,
                .scale = scale,
                .onChange = [cb = ctx.setEnabled, id = plugin.id](bool on) {
                  if (cb) {
                    cb(id, on);
                  }
                },
            })
        );
      }
      return row;
    }

    // ── Per-plugin settings editor ─────────────────────────────────────────

    std::string valueAsString(const WidgetSettingValue& value) {
      if (const auto* s = std::get_if<std::string>(&value)) {
        return *s;
      }
      if (const auto* b = std::get_if<bool>(&value)) {
        return *b ? "true" : "false";
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*i);
      }
      if (const auto* d = std::get_if<double>(&value)) {
        return std::to_string(*d);
      }
      return {};
    }

    std::vector<std::string> valueAsStringList(const WidgetSettingValue& value) {
      if (const auto* v = std::get_if<std::vector<std::string>>(&value)) {
        return *v;
      }
      return {};
    }

    WidgetSettingStringMap valueAsStringMap(const WidgetSettingValue& value) {
      if (const auto* map = std::get_if<WidgetSettingStringMap>(&value)) {
        return *map;
      }
      return {};
    }

    bool valueAsBool(const WidgetSettingValue& value) {
      if (const auto* b = std::get_if<bool>(&value)) {
        return *b;
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i != 0;
      }
      return false;
    }

    std::int64_t valueAsInt(const WidgetSettingValue& value) {
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return *i;
      }
      if (const auto* d = std::get_if<double>(&value)) {
        return static_cast<std::int64_t>(std::llround(*d));
      }
      return 0;
    }

    double valueAsDouble(const WidgetSettingValue& value) {
      if (const auto* d = std::get_if<double>(&value)) {
        return *d;
      }
      if (const auto* i = std::get_if<std::int64_t>(&value)) {
        return static_cast<double>(*i);
      }
      return 0.0;
    }

    // Current value for a plugin setting: the override if present, else the manifest default.
    WidgetSettingValue
    pluginSettingValue(const Config& cfg, const std::string& pluginId, const WidgetSettingSpec& spec) {
      const auto pluginIt = cfg.plugins.pluginSettings.find(pluginId);
      if (pluginIt != cfg.plugins.pluginSettings.end()) {
        const auto keyIt = pluginIt->second.find(spec.schema.key);
        if (keyIt != pluginIt->second.end()) {
          return keyIt->second;
        }
      }
      return spec.schema.defaultValue;
    }

    bool pluginSettingVisible(
        const Config& cfg, const std::string& pluginId, const WidgetSettingSpec& spec,
        const std::vector<WidgetSettingSpec>& allSpecs
    ) {
      if (!spec.visibleWhen.has_value()) {
        return true;
      }
      const auto currentString = [&](const std::string& key) -> std::string {
        const auto depIt =
            std::ranges::find_if(allSpecs, [&](const WidgetSettingSpec& s) { return s.schema.key == key; });
        if (depIt == allSpecs.end()) {
          return {};
        }
        return valueAsString(pluginSettingValue(cfg, pluginId, *depIt));
      };
      const auto matches = [&](const WidgetSettingVisibilityCondition& cond) {
        if (cond.nonEmpty) {
          const auto depIt =
              std::ranges::find_if(allSpecs, [&](const WidgetSettingSpec& s) { return s.schema.key == cond.key; });
          if (depIt == allSpecs.end()) {
            return false;
          }
          const WidgetSettingValue value = pluginSettingValue(cfg, pluginId, *depIt);
          if (const auto* list = std::get_if<std::vector<std::string>>(&value)) {
            return !list->empty();
          }
          if (const auto* str = std::get_if<std::string>(&value)) {
            return !str->empty();
          }
          return false;
        }
        const std::string value = currentString(cond.key);
        return std::ranges::contains(cond.values, value);
      };
      // Visible when any `any` alternative matches (or none declared) AND every `all` condition matches.
      const auto& vis = *spec.visibleWhen;
      const bool anyOk = vis.any.empty() || std::ranges::any_of(vis.any, matches);
      const bool allOk = std::ranges::all_of(vis.all, matches);
      return anyOk && allOk;
    }

    std::unique_ptr<Node> pluginSettingControl(
        SettingsControlFactory& factory, const WidgetSettingSpec& spec, const WidgetSettingValue& value,
        const std::vector<std::string>& path
    ) {
      switch (spec.control) {
      case WidgetControlKind::Bool: {
        std::optional<bool> clearWhenValue;
        if (const auto* defaultBool = std::get_if<bool>(&spec.schema.defaultValue)) {
          clearWhenValue = *defaultBool;
        }
        return factory.makeToggle(valueAsBool(value), true, path, clearWhenValue);
      }
      case WidgetControlKind::Int: {
        const double minValue = spec.schema.minValue.value_or(0.0);
        const double maxValue = spec.schema.maxValue.value_or(100.0);
        return factory.makeSlider(
            static_cast<double>(valueAsInt(value)), minValue, maxValue, spec.schema.step.value_or(1.0), path,
            /*integerValue=*/true
        );
      }
      case WidgetControlKind::Double: {
        const double minValue = spec.schema.minValue.value_or(0.0);
        const double maxValue = spec.schema.maxValue.value_or(1.0);
        return factory.makeSlider(
            valueAsDouble(value), minValue, maxValue, spec.schema.step.value_or(1.0), path, false
        );
      }
      case WidgetControlKind::Select: {
        std::vector<SelectOption> options;
        options.reserve(spec.options.size());
        for (const auto& option : spec.options) {
          options.push_back(
              SelectOption{option.value, spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)}
          );
        }
        SelectSetting selectSetting{std::move(options), valueAsString(value)};
        selectSetting.segmented = spec.segmented;
        if (spec.schema.type == noctalia::config::schema::WidgetSettingType::Bool) {
          selectSetting.valueType = SelectValueType::Boolean;
        }
        if (const auto* defaultString = std::get_if<std::string>(&spec.schema.defaultValue)) {
          selectSetting.clearOnEmpty = defaultString->empty();
        }
        return factory.makeSelect(selectSetting, path);
      }
      case WidgetControlKind::ColorSpec: {
        ColorSpecPickerSetting pickerSetting;
        pickerSetting.selectedValue = valueAsString(value);
        pickerSetting.allowNone = spec.advanced;
        pickerSetting.allowCustomColor = spec.allowCustomColor;
        return factory.makeColorSpecPicker(pickerSetting, path);
      }
      case WidgetControlKind::StringList:
      case WidgetControlKind::StringMap:
        return nullptr;
      case WidgetControlKind::File:
      case WidgetControlKind::Folder:
        return factory.makePathBrowse(
            TextSetting{
                .value = valueAsString(value),
                .placeholder = {},
                .width = 190.0F,
                .browseMode = spec.control == WidgetControlKind::Folder ? TextSettingBrowseMode::SelectFolder
                                                                        : TextSettingBrowseMode::OpenFile,
                .browseFileExtensions = spec.extensions,
                .browseFallbackDirectory = {},
            },
            path
        );
      case WidgetControlKind::Glyph: {
        const std::string currentValue = valueAsString(value);
        auto textNode = factory.makeText(currentValue, {}, path);
        return ui::row(
            {
                .align = FlexAlign::Center,
                .gap = Style::spaceSm * factory.scale(),
            },
            std::move(textNode),
            ui::button({
                .glyph = "apps",
                .glyphSize = Style::fontSizeBody * factory.scale(),
                .variant = ButtonVariant::Default,
                .minWidth = Style::controlHeight * factory.scale(),
                .minHeight = Style::controlHeight * factory.scale(),
                .paddingV = Style::spaceXs * factory.scale(),
                .paddingH = Style::spaceSm * factory.scale(),
                .radius = Style::scaledRadiusMd(factory.scale()),
                .onClick = [setOverride = factory.context().setOverride, path, currentValue]() {
                  GlyphPickerDialogOptions options;
                  if (!currentValue.empty()) {
                    options.initialGlyph = currentValue;
                  }
                  (void)GlyphPickerDialog::open(
                      std::move(options), [setOverride, path](std::optional<GlyphPickerResult> result) {
                        if (result.has_value()) {
                          setOverride(path, result->name);
                        }
                      }
                  );
                },
            })
        );
      }
      case WidgetControlKind::String:
      default:
        return factory.makeText(valueAsString(value), {}, path);
      }
    }

  } // namespace

  std::vector<PluginSettingsTab> pluginSettingsTabs(const Config& cfg) {
    std::vector<PluginSettingsTab> tabs;
    for (const auto& id : cfg.plugins.enabled) {
      const auto* manifest = scripting::PluginRegistry::instance().findManifest(id);
      if (!manifest || manifest->settingsTabs.empty()) continue;
      const auto field = std::ranges::find(manifest->settings, manifest->settingsTabs, &scripting::ManifestField::key);
      if (field == manifest->settings.end()) continue;
      scripting::PluginTranslationCatalog translations;
      if (auto dir = scripting::PluginRegistry::instance().findPluginDir(id)) translations.load(*dir);
      for (const auto& option : field->options) {
        tabs.push_back({"plugin:" + id + ":" + option.value, id, option.value,
          option.labelKey.empty() ? option.value : translations.translate(option.labelKey),
          manifest->icon.empty() ? "puzzle" : manifest->icon,
          manifest->settingsTabTargets.contains(option.value) ? manifest->settingsTabTargets.at(option.value) : "",
          std::ranges::contains(manifest->presetTabs, option.value)});
      }
    }
    return tabs;
  }

  std::vector<std::string> presetNativeSections(const Config& cfg) {
    std::vector<std::string> result;
    for (const auto& id : cfg.plugins.enabled) {
      if (const auto* manifest = scripting::PluginRegistry::instance().findManifest(id))
        for (const auto& section : manifest->presetSections)
          if (!std::ranges::contains(result, section)) result.push_back(section);
    }
    return result;
  }

  void addPresetActions(Flex& body, const Config& cfg, SettingsControlFactory& factory,
                        std::string_view section, float scale) {
    (void)cfg;
    (void)section;
    const auto& ctx = factory.context();
    if (!ctx.configService || !ctx.configService->profilePreviewDirty() || !ctx.profileAction) return;
    auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale,
                       .padding = Style::spaceSm * scale});
    const bool conflict = ctx.configService->profilePreviewConflict();
    row->addChild(makeLabel(conflict ? "Appearance changed elsewhere. Discard to reload before saving."
          : "Unsaved appearance changes", Style::fontSizeCaption * scale,
          conflict ? ColorRole::Error : ColorRole::OnSurfaceVariant));
    row->addChild(ui::spacer());
    for (const auto& [action, label] : std::vector<std::pair<std::string, std::string>>{
          {"discard", "Cancel"}, {"save", "Save"}}) {
      auto button = makeConfirmButton(label, action == "save" ? ButtonVariant::Primary : ButtonVariant::Secondary,
          scale, [callback = ctx.profileAction, action] { callback(action); });
      button->setEnabled(!ctx.profileTransitionBusy && !(conflict && action == "save"));
      if (button->inputArea()) button->inputArea()->setTabFocusKey("profile-action-" + action);
      row->addChild(std::move(button));
    }
    body.addChild(std::move(row));
  }

  std::optional<PluginSettingRoute> pluginSettingRoute(const Config& cfg, const std::vector<std::string>& path) {
    for (const auto& id : cfg.plugins.enabled) {
      const auto* manifest = scripting::PluginRegistry::instance().findManifest(id);
      if (!manifest) continue;
      const auto boolean = [&](const std::string& key) {
        const auto field = std::ranges::find(manifest->settings, key, &scripting::ManifestField::key);
        bool value = field != manifest->settings.end() && field->boolDefault;
        if (const auto plugin = cfg.plugins.pluginSettings.find(id); plugin != cfg.plugins.pluginSettings.end())
          if (const auto stored = plugin->second.find(key); stored != plugin->second.end())
            if (const auto* enabled = std::get_if<bool>(&stored->second)) value = *enabled;
        return value;
      };
      for (const auto& rule : manifest->settingsOwnership)
        if (!rule.setting.empty() && rule.path == path && boolean(rule.when))
          return PluginSettingRoute{{"plugin_settings", id, rule.setting}, boolean(rule.setting)};
    }
    return std::nullopt;
  }

  bool routePluginSettingWrites(const Config& cfg,
      std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>>& writes) {
    auto routed = writes;
    for (auto& [path, value] : routed) {
      if (const auto route = pluginSettingRoute(cfg, path)) {
        const auto* manifest = scripting::PluginRegistry::instance().findManifest(route->path[1]);
        if (manifest == nullptr) return false;
        const auto field = std::ranges::find(manifest->settings, route->path[2], &scripting::ManifestField::key);
        if (field == manifest->settings.end()) return false;
        const bool valid = [&] {
          switch (field->type) {
          case scripting::ManifestFieldType::Bool: return std::holds_alternative<bool>(value);
          case scripting::ManifestFieldType::Int: return std::holds_alternative<std::int64_t>(value);
          case scripting::ManifestFieldType::Double: return std::holds_alternative<double>(value);
          case scripting::ManifestFieldType::String:
          case scripting::ManifestFieldType::File:
          case scripting::ManifestFieldType::Folder:
          case scripting::ManifestFieldType::Glyph:
          case scripting::ManifestFieldType::Select:
          case scripting::ManifestFieldType::Color: return std::holds_alternative<std::string>(value);
          case scripting::ManifestFieldType::StringList:
            return std::holds_alternative<std::vector<std::string>>(value);
          case scripting::ManifestFieldType::StringMap: return false;
          }
          return false;
        }();
        if (!valid) return false;
        path = route->path;
      }
    }
    // A grouped control must not send contradictory values to the same owner.
    for (std::size_t i = 0; i < routed.size(); ++i) {
      for (std::size_t j = i + 1; j < routed.size();) {
        if (routed[i].first != routed[j].first) { ++j; continue; }
        if (routed[i].second != routed[j].second) return false;
        routed.erase(routed.begin() + static_cast<std::ptrdiff_t>(j));
      }
    }
    writes = std::move(routed);
    return true;
  }

  void routePluginSettingResets(const Config& cfg, std::vector<std::vector<std::string>>& paths) {
    for (auto& path : paths) if (const auto route = pluginSettingRoute(cfg, path)) path = route->path;
    std::ranges::sort(paths);
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  }

  void applyPluginSettingRoutes(const Config& cfg, std::vector<SettingEntry>& entries) {
    for (auto& entry : entries) {
      if (auto* select = std::get_if<SelectSetting>(&entry.control)) {
        if (const auto route = pluginSettingRoute(cfg, select->linkedPath)) {
          if (select->linkedBooleanValues)
            select->selectedValue = route->value ? select->linkedBooleanValues->second : select->linkedBooleanValues->first;
          select->linkedPath = route->path;
        }
        routePluginSettingResets(cfg, select->linkedPaths);
      }
    }
  }

  bool pluginOwnsSetting(const Config& cfg, const std::vector<std::string>& path) {
    for (const auto& id : cfg.plugins.enabled) {
      const auto* manifest = scripting::PluginRegistry::instance().findManifest(id);
      if (!manifest) continue;
      for (const auto& rule : manifest->settingsOwnership) {
        if (rule.path.size() != path.size()) continue;
        bool match = true;
        for (std::size_t i = 0; i < path.size(); ++i)
          if (rule.path[i] != "*" && rule.path[i] != path[i]) match = false;
        if (!match) continue;
        const auto field = std::ranges::find(manifest->settings, rule.when, &scripting::ManifestField::key);
        if (field == manifest->settings.end()) continue;
        bool enabled = field->boolDefault;
        if (const auto plugin = cfg.plugins.pluginSettings.find(id); plugin != cfg.plugins.pluginSettings.end())
          if (const auto value = plugin->second.find(rule.when); value != plugin->second.end())
            if (const auto* toggle = std::get_if<bool>(&value->second)) enabled = *toggle;
        if (enabled) return true;
      }
    }
    return false;
  }

  bool pluginHasSettings(const scripting::PluginManifest& manifest) {
    if (!manifest.settings.empty()) {
      return true;
    }
    return std::ranges::any_of(manifest.entries, [](const scripting::PluginEntry& entry) {
      return entry.kind == scripting::PluginEntryKind::Panel && !entry.settings.empty();
    });
  }

  bool buildPluginSettingsEditor(
      Flex& body, const Config& cfg, SettingsControlFactory& factory, const std::string& pluginId,
      const scripting::PluginManifest& manifest, bool showAdvanced, float scale, std::string_view tab
  ) {
    scripting::PluginTranslationCatalog translations;
    if (const auto pluginDir = scripting::PluginRegistry::instance().findPluginDir(pluginId)) {
      translations.load(*pluginDir);
    }
    std::vector<WidgetSettingSpec> specs = settings::manifestSettingSpecs(manifest.settings, &translations);
    for (const auto& entry : manifest.entries) {
      if (entry.kind != scripting::PluginEntryKind::Panel) {
        continue;
      }
      for (const auto& shellSpec : settings::pluginPanelShellSettingSpecs(entry)) {
        if (std::ranges::any_of(specs, [&](const WidgetSettingSpec& existing) {
              return existing.schema.key == shellSpec.schema.key;
            })) {
          continue;
        }
        specs.push_back(shellSpec);
      }
      for (const auto& panelSpec : settings::manifestSettingSpecs(entry.settings, &translations)) {
        if (scripting::isPanelShellSettingKey(entry.id, panelSpec.schema.key)) {
          continue;
        }
        if (std::ranges::any_of(specs, [&](const WidgetSettingSpec& existing) {
              return existing.schema.key == panelSpec.schema.key;
            })) {
          continue;
        }
        specs.push_back(panelSpec);
      }
    }
    Config pageConfig = cfg;
    if (!tab.empty()) pageConfig.plugins.pluginSettings[pluginId][manifest.settingsTabs] = std::string(tab);
    bool rendered = false;
    for (auto spec : specs) {
      if (!manifest.presetCommand.empty() && spec.schema.key == manifest.presetActions) {
        // Save/Cancel use the acknowledged native transaction controls below.
        std::erase_if(spec.options, [](const auto& option) {
          return option.value == "commit" || option.value == "cancel" || option.value == "apply";
        });
      }
      const auto field = std::ranges::find(manifest.settings, spec.schema.key, &scripting::ManifestField::key);
      if (field != manifest.settings.end() && !field->optionsFrom.empty()) {
        const auto source = std::ranges::find_if(specs, [&](const auto& item) { return item.schema.key == field->optionsFrom; });
        if (source != specs.end()) {
          spec.options.clear();
          for (const auto& name : valueAsStringList(pluginSettingValue(cfg, pluginId, *source))) {
            if (spec.options.size() == 64) break;
            spec.options.push_back({name, name});
          }
        }
      }
      if (!tab.empty() && spec.schema.key == manifest.settingsTabs) continue;
      // Automatic panel controls belong to the declared Panels destination,
      // not every tab in an integrated settings plugin.
      if (!tab.empty()) {
        const auto panelTab = std::ranges::find_if(manifest.settingsTabTargets,
            [](const auto& item) { return item.second == "panels"; });
        if (panelTab != manifest.settingsTabTargets.end() && tab != panelTab->first) {
          const bool panelField = std::ranges::any_of(manifest.entries, [&](const auto& entry) {
            return entry.kind == scripting::PluginEntryKind::Panel &&
                (scripting::isPanelShellSettingKey(entry.id, spec.schema.key) ||
                 std::ranges::any_of(entry.settings, [&](const auto& field) { return field.key == spec.schema.key; }));
          });
          if (panelField) continue;
        }
      }
      if (spec.advanced && !showAdvanced) {
        continue;
      }
      if (!pluginSettingVisible(pageConfig, pluginId, spec, specs)) {
        continue;
      }
      const std::vector<std::string> path = {"plugin_settings", pluginId, spec.schema.key};
      WidgetSettingValue value = pluginSettingValue(cfg, pluginId, spec);
      // The schema accepts legacy positions for floating panels. When this
      // panel is edge-attached, present only positions that attach to an edge.
      const auto panelEntry = std::ranges::find_if(manifest.entries, [&](const auto& candidate) {
        return candidate.kind == scripting::PluginEntryKind::Panel
            && spec.schema.key == scripting::panelShellSettingKey(candidate.id, "position");
      });
      if (panelEntry != manifest.entries.end()) {
        const auto placementKey = scripting::panelShellSettingKey(panelEntry->id, "placement");
        const auto placementSpec = std::ranges::find_if(specs, [&](const auto& candidate) {
          return candidate.schema.key == placementKey;
        });
        if (placementSpec != specs.end()
            && valueAsString(pluginSettingValue(cfg, pluginId, *placementSpec)) == "screen_edge") {
          std::erase_if(spec.options, [](const auto& option) {
            return option.value == "auto" || option.value == "center";
          });
          if (const auto* current = std::get_if<std::string>(&value);
              current != nullptr && (*current == "auto" || *current == "center")) {
            value = std::string{"bottom_center"};
          }
        }
      }
      SettingEntry entry{
          .section = SettingsSection::Bar,
          .group = "plugin-settings",
          .title = spec.literalLabel,
          .subtitle = spec.literalDescription,
          .path = path,
          .control = TextSetting{},
          .advanced = spec.advanced,
          .searchText = {},
      };
      // Optional presentation metadata groups existing scalar fields. Values and
      // persistence still belong to their ordinary plugin settings paths.
      if (field != manifest.settings.end() && field->curve) {
        const auto& group = *field->curve;
        CurveSetting curve;
        bool available = true;
        bool overridden = false;
        for (std::size_t i = 0; i < group.keys.size(); ++i) {
          const auto coordinate = std::ranges::find_if(specs, [&](const auto& item) { return item.schema.key == group.keys[i]; });
          if (coordinate == specs.end() || (coordinate->advanced && !showAdvanced) ||
              !pluginSettingVisible(pageConfig, pluginId, *coordinate, specs)) {
            available = false;
            break;
          }
          curve.paths[i] = {"plugin_settings", pluginId, group.keys[i]};
          curve.value[i] = static_cast<float>(valueAsDouble(pluginSettingValue(cfg, pluginId, *coordinate)));
          if (factory.context().configService)
            overridden |= factory.context().configService->hasEffectiveOverride(curve.paths[i]);
        }
        if (!group.activationKey.empty()) {
          const auto activation = std::ranges::find_if(specs, [&](const auto& item) { return item.schema.key == group.activationKey; });
          if (activation == specs.end()) available = false;
          else {
            curve.stylePath = {"plugin_settings", pluginId, group.activationKey};
            curve.initialStyle = valueAsString(pluginSettingValue(cfg, pluginId, *activation));
            curve.editedStyle = group.activationValue;
            if (factory.context().configService)
              overridden |= factory.context().configService->hasEffectiveOverride(curve.stylePath);
          }
        }
        SettingEntry graph = entry;
        graph.title = translations.translate(group.labelKey);
        graph.subtitle = group.descriptionKey.empty() ? std::string{} : translations.translate(group.descriptionKey);
        graph.control = curve;
        // Coordinate labels remain searchable alongside the graph label.
        for (const auto& key : group.keys) graph.searchText += " " + key;
        if (available && matchesSettingQuery(graph, factory.context().searchQuery) &&
            (!factory.context().showOverriddenOnly || !factory.context().configService || overridden)) {
          factory.makeRow(body, graph, factory.makeCurve(curve));
          rendered = true;
        }
      }
      if (field != manifest.settings.end() && field->springResponse) {
        const auto& group=*field->springResponse;
        std::array<double,3> values{}; bool available=true,overridden=false;
        for(std::size_t i=0;i<group.keys.size();++i) {
          const auto parameter=std::ranges::find_if(specs,[&](const auto& item){return item.schema.key==group.keys[i];});
          if(parameter==specs.end() || !pluginSettingVisible(pageConfig,pluginId,*parameter,specs)){available=false;break;}
          values[i]=valueAsDouble(pluginSettingValue(cfg,pluginId,*parameter));
          if(factory.context().configService) overridden|=factory.context().configService->hasEffectiveOverride({"plugin_settings",pluginId,group.keys[i]});
        }
        SettingEntry graph=entry; graph.title=translations.translate(group.labelKey);
        graph.subtitle=group.descriptionKey.empty()?std::string{}:translations.translate(group.descriptionKey);
        graph.control=SliderSetting{values[0],0.01,1000,0.01,false};
        if(available && matchesSettingQuery(graph,factory.context().searchQuery) &&
            (!factory.context().showOverriddenOnly || !factory.context().configService || overridden)) {
          factory.makeRow(body,graph,factory.makeSpringResponse(values[0],values[1],values[2])); rendered=true;
        }
      }
      if (!matchesSettingQuery(entry, factory.context().searchQuery)) continue;
      if (factory.context().showOverriddenOnly && factory.context().configService &&
          !factory.context().configService->hasEffectiveOverride(path)) continue;
      if (spec.schema.key == manifest.presetSelection && factory.context().selectPreset) {
        std::vector<SelectOption> options;
        for (const auto& option : spec.options)
          options.push_back({option.value, spec.literalLabels ? option.labelKey : i18n::tr(option.labelKey)});
        const std::string current = valueAsString(value);
        StyleGallery gallery;
        if (const auto gallerySpec = std::ranges::find_if(specs, [](const auto& item) {
              return item.schema.key == "preset_gallery";
            }); gallerySpec != specs.end()) {
          gallery = parseStyleGallery(valueAsString(pluginSettingValue(cfg, pluginId, *gallerySpec)));
        }
        auto copy = ui::column({.gap = Style::spaceXs * scale, .flexGrow = 1.0F});
        copy->addChild(makeLabel(entry.title, Style::fontSizeBody * scale, ColorRole::OnSurface, FontWeight::Bold));
        copy->addChild(makeSettingSubtitleLabel(entry.subtitle, scale));
        auto row = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale});
        row->addChild(std::move(copy));
        auto picker = ui::button({
            .text = gallery.entries.empty() ? (current.empty() ? i18n::tr("settings.plugins.style-gallery.choose") : current)
                                             : i18n::tr("settings.plugins.style-gallery.search"),
            .glyph = gallery.entries.empty() ? std::optional<std::string>{} : std::optional<std::string>{"search"},
            .enabled = !factory.context().profileTransitionBusy,
            .variant = ButtonVariant::Secondary,
            .onClick = [open = factory.context().openSearchPickerPopup,
                        select = factory.context().selectPreset, options = std::move(options),
                        pluginId, current, title = entry.title] {
              if (!open) return;
              open(SearchPickerOpenRequest{
                  .title = title, .options = options, .selectedValue = current,
                  .onSelect = [select, pluginId](const std::string& next) { select(pluginId, next); },
              });
            },
        });
        if (picker->inputArea()) picker->inputArea()->setTabFocusKey("preset-selection-" + pluginId);
        row->addChild(std::move(picker));
        body.addChild(std::move(row));
        if (!gallery.entries.empty()) {
          std::string category;
          Flex* cards = nullptr;
          for (const auto& galleryEntry : gallery.entries) {
            if (galleryEntry.category != category) {
              category = galleryEntry.category;
              const std::string heading = category == "factory" ? i18n::tr("settings.plugins.style-gallery.built-in")
                  : category == "reference" ? i18n::tr("settings.plugins.style-gallery.reference")
                  : i18n::tr("settings.plugins.style-gallery.saved");
              body.addChild(makeLabel(heading, Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant, FontWeight::Bold));
              auto group = ui::row({.align = FlexAlign::Stretch, .wrap = true, .gap = Style::spaceSm * scale, .fillWidth = true});
              cards = static_cast<Flex*>(body.addChild(std::move(group)));
            }
            auto card = galleryCard(
                galleryEntry, current, !factory.context().profileTransitionBusy, scale,
                [select = factory.context().selectPreset, pluginId, name = galleryEntry.name] { select(pluginId, name); }
            );
            if (card->inputArea()) card->inputArea()->setTabFocusKey("style-gallery-" + pluginId + "-" + galleryEntry.name);
            cards->addChild(std::move(card));
          }
        } else if (!gallery.error.empty()) {
          body.addChild(makeLabel(
              i18n::tr("settings.plugins.style-gallery.unavailable"), Style::fontSizeCaption * scale,
              ColorRole::OnSurfaceVariant
          ));
        }
        rendered = true;
        continue;
      }
      if (spec.control == WidgetControlKind::StringList) {
        factory.makeListBlock(body, entry, ListSetting{.items = valueAsStringList(value)});
      } else if (spec.control == WidgetControlKind::StringMap) {
        factory.makeStringMapBlock(
            body, entry,
            StringMapSetting{
                .entries = valueAsStringMap(value),
                .keyPlaceholder = i18n::tr("settings.widgets.map-placeholders.key"),
                .valuePlaceholder = i18n::tr("settings.widgets.map-placeholders.value"),
            }
        );
      } else {
        factory.makeRow(body, entry, pluginSettingControl(factory, spec, value, path));
      }
      rendered = true;
    }
    if (!rendered && factory.context().searchQuery.empty()) {
      body.addChild(makeLabel(
          i18n::tr("settings.plugins.settings.empty"), Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant
      ));
    }
    return rendered;
  }

  void addSettingsPlugins(Flex& content, SettingsPluginsContext ctx) {
    if (ctx.selectedSection != "plugins") {
      return;
    }
    const float scale = ctx.scale;

    auto sectionCol = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceSm * scale,
        .padding = Style::spaceLg * scale,
        .fill = clearColorSpec(),
        .fillWidth = true,
    });
    Flex* section = sectionCol.get();
    content.addChild(std::move(sectionCol));

    auto titleRow = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true},
        ui::glyph({
            .glyph = "puzzle",
            .glyphSize = Style::fontSizeHeader * scale,
            .color = colorSpecFromRole(ColorRole::Primary),
        }),
        makeLabel(
            i18n::tr("settings.navigation.sections.plugins"), Style::fontSizeHeader * scale, ColorRole::Primary,
            FontWeight::Bold
        )
    );
    if (ctx.pageTitleRow != nullptr && !ctx.searchActive && ctx.pageTitleRow->children().empty()) {
      ctx.pageTitleRow->addChild(std::move(titleRow));
    } else {
      section->addChild(std::move(titleRow));
    }

    const std::string sourcesTitle = i18n::tr("settings.plugins.sources.title");
    const std::string pluginsTitle = i18n::tr("settings.plugins.plugins.title");
    auto [pageIt, fresh] = ctx.expandedGroupsByPage.try_emplace("plugins");
    if (fresh) {
      pageIt->second.insert("plugins");
    }
    auto& expandedGroups = pageIt->second;

    Button* sourcesPill = nullptr;
    Button* pluginsPill = nullptr;
    if (ctx.groupJumpRow != nullptr && !ctx.searchActive) {
      ctx.groupJumpRow->addChild(
          ui::button({
              .out = &sourcesPill,
              .text = sourcesTitle,
              .fontSize = Style::fontSizeCaption * scale,
              .variant = expandedGroups.contains("sources") ? ButtonVariant::Primary : ButtonVariant::Default,
              .radius = Style::scaledRadiusMd(scale),
          })
      );
      ctx.groupJumpRow->addChild(
          ui::button({
              .out = &pluginsPill,
              .text = pluginsTitle,
              .fontSize = Style::fontSizeCaption * scale,
              .variant = expandedGroups.contains("plugins") ? ButtonVariant::Primary : ButtonVariant::Default,
              .radius = Style::scaledRadiusMd(scale),
          })
      );
    }

    if (ctx.config != nullptr && ctx.config->shell.offlineMode) {
      section->addChild(makeOfflineModeNotice(scale, i18n::tr("settings.window.offline-mode-notice.plugins")));
    }

    Flex* sourcesBody = addSettingsGroupCard(
        SettingsGroupCardProps{
            .parent = *section,
            .group = "sources",
            .title = sourcesTitle,
            .scale = scale,
            .expandedGroups = expandedGroups,
            .pill = sourcesPill,
            .scrollToTop = ctx.scrollContentToTop,
        }
    );

    Flex* sourcesHeader = nullptr;
    auto sourcesHeaderNode = ui::row({
        .out = &sourcesHeader,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    sourcesHeader->addChild(ui::spacer());
    sourcesHeader->addChild(
        ui::button({
            .text = i18n::tr("settings.plugins.sources.add"),
            .glyph = "add",
            .fontSize = Style::fontSizeCaption * scale,
            .glyphSize = Style::fontSizeBody * scale,
            .variant = ButtonVariant::Default,
            .onClick = [cb = ctx.addSource]() {
              if (cb) {
                cb();
              }
            },
        })
    );
    sourcesBody->addChild(std::move(sourcesHeaderNode));
    if (ctx.sources.empty()) {
      sourcesBody->addChild(makeLabel(
          i18n::tr("settings.plugins.sources.empty"), Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant
      ));
    } else if (ctx.sources.size() > 1) {
      sourcesBody->addChild(makeLabel(
          i18n::tr("settings.plugins.sources.precedence-hint"), Style::fontSizeCaption * scale,
          ColorRole::OnSurfaceVariant
      ));
    }
    // Render in config order so the list mirrors the file. Precedence is last-wins
    // (the same cascade as the rest of the config), so a source lower in the list
    // overrides the ones above it for a shared plugin id.
    for (const auto& source : ctx.sources) {
      sourcesBody->addChild(sourceRow(source, ctx, scale));
    }

    const bool hasGitSource = std::ranges::any_of(ctx.sources, [](const PluginSourceConfig& s) {
      return s.kind == PluginSourceKind::Git && s.enabled;
    });
    if (hasGitSource && ctx.setAutoUpdate) {
      auto autoRow = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
      auto autoInfo = ui::column({.align = FlexAlign::Start, .gap = 2.0F * scale, .flexGrow = 1.0F});
      autoInfo->addChild(makeLabel(
          i18n::tr("settings.plugins.sources.auto-update"), Style::fontSizeBody * scale, ColorRole::OnSurface,
          FontWeight::Medium
      ));
      autoInfo->addChild(makeLabel(
          i18n::tr("settings.plugins.sources.auto-update-desc"), Style::fontSizeCaption * scale,
          ColorRole::OnSurfaceVariant
      ));
      std::vector<ui::SegmentedOption> modeOptions;
      modeOptions.reserve(std::size(kPluginAutoUpdateModes));
      std::optional<std::size_t> selectedModeIndex;
      for (const auto& opt : kPluginAutoUpdateModes) {
        if (opt.value == ctx.autoUpdateMode) {
          selectedModeIndex = modeOptions.size();
        }
        modeOptions.push_back(ui::SegmentedOption{.label = i18n::tr(opt.labelKey)});
      }
      autoRow->addChild(std::move(autoInfo));
      autoRow->addChild(
          ui::segmented({
              .options = std::move(modeOptions),
              .selectedIndex = selectedModeIndex,
              .scale = scale,
              .onChange = [cb = ctx.setAutoUpdate](std::size_t index) {
                if (cb && index < std::size(kPluginAutoUpdateModes)) {
                  cb(kPluginAutoUpdateModes[index].value);
                }
              },
          })
      );
      sourcesBody->addChild(std::move(autoRow));
    }

    // ── Plugins ──────────────────────────────────────────────────────────
    Flex* pluginsBody = addSettingsGroupCard(
        SettingsGroupCardProps{
            .parent = *section,
            .group = "plugins",
            .title = pluginsTitle,
            .scale = scale,
            .expandedGroups = expandedGroups,
            .pill = pluginsPill,
            .scrollToTop = ctx.scrollContentToTop,
        }
    );

    Flex* pluginsHeader = nullptr;
    auto pluginsHeaderNode = ui::row({
        .out = &pluginsHeader,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    if (ctx.pluginsLoading) {
      pluginsHeader->addChild(
          ui::spinner({
              .spinnerSize = Style::fontSizeBody * scale,
              .spinning = true,
          })
      );
    }
    pluginsHeader->addChild(ui::spacer());
    const int updatesAvailable = static_cast<int>(
        std::ranges::count_if(ctx.plugins, [](const scripting::PluginStatus& p) { return p.updateAvailable; })
    );
    if (updatesAvailable > 0 && ctx.updateAll) {
      pluginsHeader->addChild(
          ui::button({
              .text = i18n::tr("settings.plugins.plugins.update-all", "count", std::to_string(updatesAvailable)),
              .glyph = "download",
              .fontSize = Style::fontSizeCaption * scale,
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Primary,
              .onClick = [cb = ctx.updateAll]() {
                if (cb) {
                  cb();
                }
              },
          })
      );
    }
    if (ctx.openStore) {
      pluginsHeader->addChild(
          ui::button({
              .text = i18n::tr("settings.plugins.browse-store"),
              .glyph = "search",
              .fontSize = Style::fontSizeCaption * scale,
              .glyphSize = Style::fontSizeBody * scale,
              .variant = ButtonVariant::Primary,
              .onClick = [cb = ctx.openStore]() {
                if (cb) {
                  cb();
                }
              },
          })
      );
    }
    pluginsBody->addChild(std::move(pluginsHeaderNode));
    if (!ctx.pluginsLoading && ctx.plugins.empty()) {
      pluginsBody->addChild(makeLabel(
          i18n::tr("settings.plugins.plugins.empty"), Style::fontSizeCaption * scale, ColorRole::OnSurfaceVariant
      ));
    }
    std::vector<scripting::PluginStatus> plugins;
    plugins.reserve(ctx.plugins.size());
    for (const auto& plugin : ctx.plugins) {
      if (plugin.materialized || plugin.enabled) {
        plugins.push_back(plugin);
      }
    }
    std::ranges::sort(plugins, [&](const auto& a, const auto& b) {
      const std::string_view aName = pluginDisplayName(a);
      const std::string_view bName = pluginDisplayName(b);
      if (aName != bName) {
        return aName < bName;
      }
      if (a.source != b.source) {
        return pluginSourceLess(a.source, b.source);
      }
      return a.id < b.id;
    });
    for (const auto& plugin : plugins) {
      pluginsBody->addChild(pluginRow(plugin, ctx, scale));
      if (!ctx.pendingDeletePluginId.empty() && ctx.pendingDeletePluginId == plugin.id) {
        pluginsBody->addChild(pluginDeleteConfirmPanel(plugin, ctx, scale));
      }
    }
  }

} // namespace settings
