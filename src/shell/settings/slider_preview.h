#pragma once

#include "core/timer_manager.h"
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace settings {
// A pointer can produce hundreds of changes before configuration resolution
// finishes. Retain only the latest preview and always flush the released value.
template <class Value>
class SettingPreview : public std::enable_shared_from_this<SettingPreview<Value>> {
public:
  explicit SettingPreview(std::function<void(Value)> commit) : m_commit(std::move(commit)) {}

  void queue(Value value) {
    m_pending = value;
    if (m_timer.active()) return;
    const auto weak = this->weak_from_this();
    m_timer.start(std::chrono::milliseconds(16), [weak] {
      if (const auto self = weak.lock()) self->flush();
    });
  }

  void finish(Value value) {
    m_timer.stop();
    m_pending = value;
    flush();
  }

private:
  void flush() {
    const auto value = std::exchange(m_pending, std::nullopt);
    if (!value || value == m_last) return;
    m_last = value;
    m_commit(*value);
  }

  Timer m_timer;
  std::optional<Value> m_pending, m_last;
  std::function<void(Value)> m_commit;
};
using SliderPreview = SettingPreview<double>;
}
