#pragma once

#include "ui/signal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Style {

struct MaterialTargetDescriptor {
  std::string id;
  std::string label;
  std::string role;
  std::string family;
  // Broad to specific. The final entry is the descriptor's public target ID.
  std::vector<std::string> surfaces;
  bool operator==(const MaterialTargetDescriptor&) const = default;
};

struct RegisteredMaterialTarget {
  MaterialTargetDescriptor descriptor;
  std::size_t liveInstances = 0;
  bool builtIn = false;
};

class MaterialTargetCatalog;

class MaterialTargetRegistration {
public:
  MaterialTargetRegistration() = default;
  ~MaterialTargetRegistration();

  MaterialTargetRegistration(const MaterialTargetRegistration&) = delete;
  MaterialTargetRegistration& operator=(const MaterialTargetRegistration&) = delete;
  MaterialTargetRegistration(MaterialTargetRegistration&& other) noexcept;
  MaterialTargetRegistration& operator=(MaterialTargetRegistration&& other) noexcept;

  void reset() noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return m_catalog != nullptr; }

private:
  friend class MaterialTargetCatalog;
  MaterialTargetRegistration(MaterialTargetCatalog* catalog, std::uint64_t token) noexcept;

  MaterialTargetCatalog* m_catalog = nullptr;
  std::uint64_t m_token = 0;
};

class MaterialTargetCatalog {
public:
  [[nodiscard]] static MaterialTargetCatalog& instance();

  // Registers a live owner with a durable public identity. Reusing an ID is
  // accepted only when every descriptor field matches; conflicting metadata is
  // rejected. The token is private bookkeeping and never forms a public ID.
  [[nodiscard]] MaterialTargetRegistration registerInstance(MaterialTargetDescriptor descriptor);
  [[nodiscard]] MaterialTargetRegistration registerClassTarget(std::string_view id);

  [[nodiscard]] std::vector<RegisteredMaterialTarget> snapshot() const;
  [[nodiscard]] std::optional<RegisteredMaterialTarget> find(std::string_view id) const;
  [[nodiscard]] bool supported(std::string_view id) const;
  [[nodiscard]] Signal<>& changed() noexcept { return m_changed; }

private:
  friend class MaterialTargetRegistration;
  MaterialTargetCatalog();
  void unregister(std::uint64_t token) noexcept;

  struct State;
  std::vector<State> m_targets;
  std::vector<std::pair<std::uint64_t, std::string>> m_tokens;
  std::uint64_t m_nextToken = 0;
  Signal<> m_changed;
};

// Returns baseSurface followed by the registered target's broad-to-specific
// path, with adjacent duplicates removed. Unknown targets yield only the base.
[[nodiscard]] std::vector<std::string>
materialTargetSurfacePath(std::string_view baseSurface, std::string_view targetId);

} // namespace Style
