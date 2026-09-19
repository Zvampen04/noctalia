#pragma once

#include <nlohmann/json_fwd.hpp>

namespace text {
  // Event-loop thread only. Validates the complete request before registration.
  // Imported font bytes are retained for the lifetime of the process; loading
  // resources never selects a family or changes an appearance transaction.
  nlohmann::json registerProfileFontResources(const nlohmann::json& records);
}
