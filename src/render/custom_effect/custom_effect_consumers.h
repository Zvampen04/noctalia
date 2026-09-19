#pragma once

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// Tracks effects referenced by each rendered surface. Configuration may keep
// dormant overrides; only current surface scenes retain compiled resources.
class CustomEffectConsumers {
public:
  using SurfaceId = std::uintptr_t;
  using EffectSet = std::set<std::string, std::less<>>;

  [[nodiscard]] std::vector<std::string> update(SurfaceId surface, EffectSet effects) {
    std::vector<std::string> released;
    const auto found = m_surfaces.find(surface);
    if (found != m_surfaces.end()) {
      for (const auto& stableId : found->second) {
        if (effects.contains(stableId)) continue;
        const auto count = m_references.find(stableId);
        if (count != m_references.end() && --count->second == 0) {
          released.push_back(count->first);
          m_references.erase(count);
        }
      }
    }
    for (const auto& stableId : effects) {
      if (found != m_surfaces.end() && found->second.contains(stableId)) continue;
      ++m_references[stableId];
    }
    if (effects.empty()) m_surfaces.erase(surface);
    else m_surfaces.insert_or_assign(surface, std::move(effects));
    return released;
  }

  [[nodiscard]] std::vector<std::string> remove(SurfaceId surface) { return update(surface, {}); }

  [[nodiscard]] std::size_t references(std::string_view stableId) const {
    const auto found = m_references.find(std::string(stableId));
    return found == m_references.end() ? 0 : found->second;
  }
  [[nodiscard]] std::size_t surfaces() const noexcept { return m_surfaces.size(); }

private:
  std::unordered_map<SurfaceId, EffectSet> m_surfaces;
  std::unordered_map<std::string, std::size_t> m_references;
};
