// Include inside buildSettingsRegistry, where cfg, entries and makeEntry exist.
{
  const auto controlSelect = []<class T>(T value) {
    SelectSetting selection;
    for (const auto key : Style::controlKeys<T>())
      selection.options.push_back({.value = std::string(key), .label = std::string(key)});
    selection.selectedValue = Style::controlName(value);
    return selection;
  };
#define CONTROL_ENUM(member, type, initial, label, group) \
  entries.push_back(makeEntry(SettingsSection::Appearance, "controls-" group, label, \
      "Configurable native control presentation using the current palette.", \
      {"shell", "controls", #member}, controlSelect(cfg.shell.controls.member), "control presentation " group, true));
#define CONTROL_NUMBER(member, initial, low, high, step, label, group) \
  entries.push_back(makeEntry(SettingsSection::Appearance, "controls-" group, label, \
      "Changes preview immediately. Zero is an explicit value.", \
      {"shell", "controls", #member}, SliderSetting{cfg.shell.controls.member, low, high, step, false}, "control geometry " group, true));
#define CONTROL_BOOL(member, initial, label, group) \
  entries.push_back(makeEntry(SettingsSection::Appearance, "controls-" group, label, \
      "Changes preview immediately.", {"shell", "controls", #member}, ToggleSetting{cfg.shell.controls.member}, "control " group, true));
#include "ui/control_settings.def"
#undef CONTROL_ENUM
#undef CONTROL_NUMBER
#undef CONTROL_BOOL
}
