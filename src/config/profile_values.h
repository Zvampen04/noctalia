#pragma once

#include "config/profile_scope.h"
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace noctalia::profile {
using Json = nlohmann::json;
inline Json atPath(const Json& object, const Path& path) {
  const auto* node = &object;
  for (const auto& key : path) {
    if (!node->is_object() || !node->contains(key)) return nullptr;
    node = &node->at(key);
  }
  return *node;
}
inline bool equalValue(const Json& a, const Json& b) {
  if (a.is_number() && b.is_number()) {
    const double x = a.get<double>(), y = b.get<double>();
    return std::abs(x-y) <= 1e-6 * std::max({1., std::abs(x), std::abs(y)});
  }
  if (a.type() != b.type() || a.size() != b.size()) return false;
  if (a.is_object()) {
    for (auto it = a.begin(); it != a.end(); ++it)
      if (!b.contains(it.key()) || !equalValue(it.value(), b.at(it.key()))) return false;
    return true;
  }
  if (a.is_array()) {
    for (std::size_t i = 0; i < a.size(); ++i) if (!equalValue(a[i], b[i])) return false;
    return true;
  }
  return a == b;
}
inline void putValue(toml::table& table, const std::string& key, const Json& value, int depth = 0) {
  if (depth > 16) throw std::runtime_error("Profile value nesting exceeds 16 levels");
  if (value.is_null()) table.erase(key);
  else if (value.is_boolean()) table.insert_or_assign(key, value.get<bool>());
  else if (value.is_number_integer()) table.insert_or_assign(key, value.get<std::int64_t>());
  else if (value.is_number_float() && std::isfinite(value.get<double>())) table.insert_or_assign(key, value.get<double>());
  else if (value.is_string()) table.insert_or_assign(key, value.get<std::string>());
  else if (value.is_object()) {
    toml::table child;
    for (auto it = value.begin(); it != value.end(); ++it) putValue(child, it.key(), it.value(), depth + 1);
    table.insert_or_assign(key, std::move(child));
  } else if (value.is_array()) {
    toml::array child;
    for (const auto& item : value) {
      toml::table holder;
      putValue(holder, "item", item, depth + 1);
      if (!holder.contains("item")) throw std::runtime_error("Profile arrays cannot contain null");
      child.push_back(*holder.get("item"));
    }
    table.insert_or_assign(key, std::move(child));
  } else throw std::runtime_error("Unsupported profile value");
}
inline Path entryPath(const Json& entry, bool profilesOnly = true) {
  auto path = entry.at("path").get<Path>();
  if (path.empty() || path.size() > 12 || (profilesOnly ? !owns(path)
      : (path.size() != 3 || path[0] != "plugin_settings" || owns(path)))) throw std::runtime_error("Property does not belong to appearance profiles");
  for (const auto& key : path) if (key.empty() || key.size() > 256) throw std::runtime_error("Invalid profile path");
  return path;
}
inline void validateExpected(const Json& live, const Json& expected, bool profilesOnly = true) {
  for (const auto& entry : expected)
    if (!equalValue(atPath(live, entryPath(entry, profilesOnly)), entry.at("value")))
      throw std::runtime_error("Appearance changed elsewhere; reload before applying");
}
inline void applyEntries(toml::table& table, const Json& entries, bool profilesOnly = true) {
  std::set<Path> seen;
  for (const auto& entry : entries) {
    const auto path = entryPath(entry, profilesOnly);
    for (const auto& previous : seen) {
      const auto count = std::min(previous.size(), path.size());
      if (std::equal(previous.begin(), previous.begin() + count, path.begin()))
        throw std::runtime_error("Profile entries overlap");
    }
    seen.insert(path);
    auto* target = &table;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
      if (!target->contains(path[i])) {
        if (entry.at("value").is_null()) { target = nullptr; break; }
        target->insert(path[i], toml::table{});
      }
      target = (*target)[path[i]].as_table();
      if (!target) throw std::runtime_error("Profile path crosses a scalar value");
    }
    if (target) putValue(*target, path.back(), entry.at("value"));
  }
}
}
