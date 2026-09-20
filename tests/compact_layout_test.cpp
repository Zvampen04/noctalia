#include "shell/control_center/compact_layout.h"

#include <cassert>
#include <limits>

int main() {
  using namespace compact_layout;
  for (int columns = 4; columns <= 12; ++columns) {
    const auto cells = defaults(columns);
    assert(parse(encode(cells), columns) == cells);
    for (const auto& cell : cells)
      assert(fits(cells, cell, columns));
    const auto packed = tidy(cells, columns);
    assert(packed.size() == cells.size());
    for (const auto& cell : packed)
      assert(fits(packed, cell, columns));
  }
  assert(parse(encode({}), 6).empty());
  assert(!parse({}, 6).empty());
  const auto safe = parse(
      {"wifi:0:0:3:1", "bluetooth:1:0:3:1", "wifi:0:3:3:1", "unknown:0:1:6:1", "volume:0:1:6:1 trailing",
       "tray:0:12:6:1", "actions:2147483647:0:6:1", "notifications:0:2:6:2"},
      6
  );
  assert(safe.size() == 2 && safe[0].kind == "wifi" && safe[1].kind == "notifications");
  assert(!fits({}, {"wifi", 0, 0, std::numeric_limits<int>::max(), 1}, 6));
  const std::vector<Cell> sparse{{"wifi", 0, 3, 3, 1}, {"bluetooth", 3, 7, 3, 1}};
  const auto packed = tidy(sparse, 6);
  assert(packed[0].y == 0 && packed[1].y == 0 && packed[1].x == 3);
  const std::vector<Cell> full{{"notifications", 0, 0, 6, 12}, {"media", 0, 0, 6, 3}};
  assert(tidy(full, 6) == full); // Never silently drop a control.
}
