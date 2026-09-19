#include "shell/bar/widgets/control_center_widget_definition.h"

#include "shell/bar/widgets/glyph_button_definition.h"

const noctalia::bar::WidgetDefinition<ControlCenterWidget::Options>& controlCenterWidgetDefinition() {
  using noctalia::bar::field;
  using Options = ControlCenterWidget::Options;

  static const settings::WidgetSettingVisibility ringEnabled{"ring", {"true"}};

  static const noctalia::bar::WidgetDefinition<Options> definition{
      .type = "control-center",
      .fields = noctalia::bar::glyphButtonFields<Options>(
          field<&Options::ring>({.key = "ring"}),
          field<&Options::ringSource>({
              .key = "ring_source",
              .choices = {
                  {.value = ControlCenterWidget::RingSource::Battery, .configValue = "battery", .labelKey = "settings.widgets.options.battery"},
                  {.value = ControlCenterWidget::RingSource::Ram, .configValue = "ram", .labelKey = "settings.widgets.options.ram-percent"},
                  {.value = ControlCenterWidget::RingSource::Cpu, .configValue = "cpu", .labelKey = "settings.widgets.options.cpu-usage"},
                  {.value = ControlCenterWidget::RingSource::Gpu, .configValue = "gpu", .labelKey = "settings.widgets.options.gpu-usage"},
              },
              .presentation = settings::WidgetSettingPresentation{.visibleWhen = ringEnabled},
          }),
          field<&Options::iconSource>({
              .key = "icon_source",
              .choices = {
                  {.value = ControlCenterWidget::IconSource::Static, .configValue = "static", .labelKey = "settings.widgets.options.static-icon"},
                  {.value = ControlCenterWidget::IconSource::Wifi, .configValue = "wifi", .labelKey = "control-center.shortcuts.wifi"},
                  {.value = ControlCenterWidget::IconSource::SystemUpdates, .configValue = "system_updates", .labelKey = "settings.widgets.options.system-updates"},
              },
          }),
          field<&Options::iconSize>({.key = "icon_size", .minValue = 0.0, .maxValue = 64.0, .step = 1.0})),
      .glyph = [](const Options& options) { return options.glyph; },
  };
  return definition;
}
