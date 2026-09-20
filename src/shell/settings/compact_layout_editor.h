#pragma once
#include <memory>
class Flex;
namespace settings {
  struct SettingsContentContext;
  std::unique_ptr<Flex> makeCompactLayoutEditor(const SettingsContentContext& context);
} // namespace settings
