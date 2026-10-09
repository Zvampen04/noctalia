#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace system_update_status {
  enum class State : std::uint8_t { Unknown, Current, Available, Failed };

  inline std::string text(const nlohmann::json& source, const char* key) {
    const auto it = source.find(key);
    return it != source.end() && it->is_string() ? it->get<std::string>() : std::string{};
  }

  inline bool flag(const nlohmann::json& source, const char* key) {
    const auto it = source.find(key);
    return it != source.end() && it->is_boolean() && it->get<bool>();
  }

  inline bool requiresReboot(const nlohmann::json& source) {
    const auto activation = source.find("activation");
    return flag(source, "reboot_required")
        || (activation != source.end() && flag(*activation, "reboot_required"));
  }

  inline bool hasFailures(const nlohmann::json& source) {
    const auto items = source.find("items");
    if (items == source.end() || !items->is_array()) return false;
    for (const auto& item : *items) {
      const auto state = text(item, "status");
      if (state == "failed" || state == "unavailable") return true;
    }
    return false;
  }

  inline bool hasActionableItems(const nlohmann::json& source, bool settled, std::int64_t now) {
    const auto items = source.find("items");
    if (items == source.end() || !items->is_array()) return false;
    for (const auto& item : *items) {
      const auto state = text(item, "status");
      const auto eligible = item.find("deferred_until_epoch");
      if (state == "deferred" && eligible != item.end() && eligible->is_number_integer()
          && eligible->get<std::int64_t>() <= now) return true;
      if (!settled && (state == "available" || state == "selected" || state == "accepted" || state == "staged"))
        return true;
    }
    return false;
  }

  inline State classify(const nlohmann::json& status, const nlohmann::json& session,
                        const nlohmann::json& prompt, std::int64_t now) {
    const auto state = text(status, "status");
    const auto phase = text(status, "phase");
    const bool settled = state == "success"
        && (phase == "up-to-date" || phase == "no-changes" || phase == "switched" || phase == "completed");
    // A fresh completed check clears historical failures, but any failed item
    // in the current check still outranks a successful partial transaction.
    if (state == "failed" || hasFailures(status) || (!settled && (text(session, "status") == "failed" || hasFailures(session)))) return State::Failed;
    const auto items = status.find("items");
    const auto& itemSource = items != status.end() && items->is_array() && !items->empty() ? status : session;
    if (requiresReboot(status) || requiresReboot(session) || !prompt.empty()
        || (state == "success" && (phase == "staged" || phase == "scheduled"))
        || hasActionableItems(itemSource, settled, now)) return State::Available;
    if (state == "success") return State::Current;
    return State::Unknown;
  }

  class Tracker {
  public:
    State update(const nlohmann::json& status, const nlohmann::json& session,
                 const nlohmann::json& prompt, std::int64_t now) {
      const auto state = classify(status, session, prompt, now);
      if (state == State::Failed) m_failed = true;
      else if (text(status, "status") == "success") m_failed = false;
      return m_failed ? State::Failed : state;
    }
  private:
    bool m_failed = false;
  };

  inline constexpr std::string_view color(State state) {
    switch (state) {
      case State::Current: return "#ffffff";
      case State::Failed: return "#ff0000";
      case State::Available:
      case State::Unknown: return "#ffff00";
    }
    return "#ffff00";
  }
}
