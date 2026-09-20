#pragma once
#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Theme-owned grid; service state and action definitions stay with their services.
namespace compact_layout {
  inline constexpr std::array<std::string_view, 8> kinds{"wifi",          "bluetooth", "volume", "brightness",
                                                         "notifications", "media",     "tray",   "actions"};
  struct Cell {
    std::string kind;
    int x = 0, y = 0, w = 6, h = 1;
    bool operator==(const Cell&) const = default;
  };
  inline bool known(std::string_view kind) { return std::ranges::find(kinds, kind) != kinds.end(); }
  inline std::vector<Cell> defaults(int columns = 6) {
    columns = std::clamp(columns, 4, 12);
    return {{"wifi", 0, 0, columns / 2, 1},      {"bluetooth", columns / 2, 0, columns - columns / 2, 1},
            {"volume", 0, 1, columns, 1},        {"brightness", 0, 2, columns, 1},
            {"notifications", 0, 3, columns, 2}, {"tray", 0, 5, columns, 1},
            {"actions", 0, 6, columns, 1}};
  }
  inline bool overlaps(const Cell& a, const Cell& b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
  }
  inline bool fits(const std::vector<Cell>& cells, const Cell& cell, int columns) {
    if (cell.x < 0
        || cell.x > columns
        || cell.y < 0
        || cell.y > 12
        || cell.w < 2
        || cell.w > columns
        || cell.h < 1
        || cell.h > 12
        || cell.x > columns - cell.w
        || cell.y > 12 - cell.h)
      return false;
    return std::ranges::none_of(cells, [&](const Cell& other) {
      return other.kind != cell.kind && overlaps(cell, other);
    });
  }
  inline std::vector<Cell> parse(const std::vector<std::string>& values, int columns) {
    if (values.empty())
      return defaults(columns);
    std::vector<Cell> cells;
    for (auto text : values) {
      std::replace(text.begin(), text.end(), ':', ' ');
      std::istringstream in(text);
      Cell c;
      std::string extra;
      if (!(in >> c.kind >> c.x >> c.y >> c.w >> c.h) || in >> extra || !known(c.kind))
        continue;
      if (std::ranges::any_of(cells, [&](const Cell& a) { return a.kind == c.kind; }))
        continue;
      if (fits(cells, c, columns))
        cells.push_back(c);
    }
    return cells;
  }
  inline std::vector<std::string> encode(const std::vector<Cell>& cells) {
    std::vector<std::string> values;
    for (const auto& c : cells)
      values.push_back(
          c.kind
          + ":"
          + std::to_string(c.x)
          + ":"
          + std::to_string(c.y)
          + ":"
          + std::to_string(c.w)
          + ":"
          + std::to_string(c.h)
      );
    // Explicit empty is distinct from factory/default layout.
    if (values.empty())
      values.push_back("empty");
    return values;
  }
  inline int rows(const std::vector<Cell>& cells) {
    int result = 1;
    for (const auto& c : cells)
      result = std::max(result, c.y + c.h);
    return result;
  }
  inline std::vector<Cell> tidy(std::vector<Cell> cells, int columns) {
    std::vector<Cell> result;
    for (auto c : cells) {
      c.w = std::clamp(c.w, 2, columns);
      bool placed = false;
      for (int y = 0; y + c.h <= 12 && !placed; ++y)
        for (int x = 0; x + c.w <= columns; ++x) {
          c.x = x;
          c.y = y;
          if (fits(result, c, columns)) {
            result.push_back(c);
            placed = true;
            break;
          }
        }
      if (!placed)
        return cells; // Do not silently delete a control when the grid is full.
    }
    return result;
  }
} // namespace compact_layout
