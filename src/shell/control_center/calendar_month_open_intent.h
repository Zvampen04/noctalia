#pragma once

#include "ui/controls/calendar_view.h"

#include <optional>

class CalendarMonthOpenIntent {
public:
  void request(calendar_view::Date selected) {
    if (selected.valid()) m_selected = selected;
  }

  [[nodiscard]] bool pending() const noexcept { return m_selected.has_value(); }

  [[nodiscard]] bool consume(
      calendar_view::Date current, calendar_view::Date& selected, int& monthOffset
  ) noexcept {
    if (!m_selected || !current.valid()) return false;
    selected = *m_selected;
    monthOffset = (selected.year - current.year) * 12 + selected.month - current.month;
    m_selected.reset();
    return true;
  }

private:
  std::optional<calendar_view::Date> m_selected;
};
