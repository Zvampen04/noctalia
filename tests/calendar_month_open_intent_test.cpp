#include "shell/control_center/calendar_month_open_intent.h"

#include <cassert>

int main() {
  CalendarMonthOpenIntent intent;
  calendar_view::Date selected;
  int monthOffset = 99;

  assert(!intent.consume({.year = 2026, .month = 8, .day = 19}, selected, monthOffset));
  assert(monthOffset == 99);

  intent.request({.year = 2027, .month = 0, .day = 3});
  assert(intent.pending());
  assert(intent.consume({.year = 2026, .month = 11, .day = 31}, selected, monthOffset));
  assert((selected == calendar_view::Date{.year = 2027, .month = 0, .day = 3}));
  assert(monthOffset == 1);
  assert(!intent.pending());
  assert(!intent.consume({.year = 2026, .month = 11, .day = 31}, selected, monthOffset));

  intent.request({.year = 2025, .month = 9, .day = 12});
  assert(intent.consume({.year = 2026, .month = 1, .day = 1}, selected, monthOffset));
  assert(monthOffset == -4);
}
