#include "ui/material_target_catalog.h"

#include "ui/material_overrides.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Style {
namespace {

const std::array kBuiltInTargets{
    MaterialTargetDescriptor{"popup", "All popups", "surface", "container", {"popup"}},
    MaterialTargetDescriptor{
        "popup.context-menu", "Context menus", "surface", "container", {"popup", "popup.context-menu"}},
    MaterialTargetDescriptor{
        "popup.select-dropdown", "Select dropdowns", "surface", "container",
        {"popup", "popup.context-menu", "popup.select-dropdown"}},
    MaterialTargetDescriptor{"popup.dialog", "Dialog popups", "surface", "panel", {"popup", "popup.dialog"}},
};

bool validDescriptor(const MaterialTargetDescriptor& descriptor) {
  if (!validMaterialTarget(descriptor.id) || descriptor.label.empty()
      || !validMaterialTarget(descriptor.role) || !validMaterialTarget(descriptor.family)
      || descriptor.surfaces.empty() || descriptor.surfaces.back() != descriptor.id) {
    return false;
  }
  return std::ranges::all_of(descriptor.surfaces, validMaterialTarget);
}

} // namespace

struct MaterialTargetCatalog::State {
  MaterialTargetDescriptor descriptor;
  std::size_t liveInstances = 0;
  bool builtIn = false;
};

MaterialTargetRegistration::MaterialTargetRegistration(MaterialTargetCatalog* catalog, std::uint64_t token) noexcept
    : m_catalog(catalog), m_token(token) {}

MaterialTargetRegistration::~MaterialTargetRegistration() { reset(); }

MaterialTargetRegistration::MaterialTargetRegistration(MaterialTargetRegistration&& other) noexcept
    : m_catalog(std::exchange(other.m_catalog, nullptr)), m_token(std::exchange(other.m_token, 0)) {}

MaterialTargetRegistration& MaterialTargetRegistration::operator=(MaterialTargetRegistration&& other) noexcept {
  if (this != &other) {
    reset();
    m_catalog = std::exchange(other.m_catalog, nullptr);
    m_token = std::exchange(other.m_token, 0);
  }
  return *this;
}

void MaterialTargetRegistration::reset() noexcept {
  if (m_catalog != nullptr) m_catalog->unregister(m_token);
  m_catalog = nullptr;
  m_token = 0;
}

MaterialTargetCatalog& MaterialTargetCatalog::instance() {
  static MaterialTargetCatalog catalog;
  return catalog;
}

MaterialTargetCatalog::MaterialTargetCatalog() {
  m_targets.reserve(kBuiltInTargets.size());
  for (const auto& descriptor : kBuiltInTargets)
    m_targets.push_back(State{.descriptor = descriptor, .builtIn = true});
}

MaterialTargetRegistration MaterialTargetCatalog::registerInstance(MaterialTargetDescriptor descriptor) {
  if (!validDescriptor(descriptor)) return {};
  auto target = std::ranges::find(m_targets, descriptor.id, [](const State& state) {
    return state.descriptor.id;
  });
  if (target != m_targets.end()) {
    if (target->descriptor != descriptor) return {};
  } else {
    target = m_targets.insert(m_targets.end(), State{.descriptor = std::move(descriptor)});
  }
  ++target->liveInstances;
  const auto token = ++m_nextToken;
  m_tokens.emplace_back(token, target->descriptor.id);
  m_changed.emit();
  return MaterialTargetRegistration(this, token);
}

MaterialTargetRegistration MaterialTargetCatalog::registerClassTarget(std::string_view id) {
  const auto target = std::ranges::find(m_targets, id, [](const State& state) {
    return std::string_view(state.descriptor.id);
  });
  if (target == m_targets.end() || !target->builtIn) return {};
  return registerInstance(target->descriptor);
}

std::vector<RegisteredMaterialTarget> MaterialTargetCatalog::snapshot() const {
  std::vector<RegisteredMaterialTarget> result;
  result.reserve(m_targets.size());
  for (const auto& target : m_targets) {
    if (!target.builtIn && target.liveInstances == 0) continue;
    result.push_back(
        {.descriptor = target.descriptor, .liveInstances = target.liveInstances, .builtIn = target.builtIn});
  }
  return result;
}

std::optional<RegisteredMaterialTarget> MaterialTargetCatalog::find(std::string_view id) const {
  const auto target = std::ranges::find(m_targets, id, [](const State& state) {
    return std::string_view(state.descriptor.id);
  });
  if (target == m_targets.end() || (!target->builtIn && target->liveInstances == 0)) return std::nullopt;
  return RegisteredMaterialTarget{
      .descriptor = target->descriptor, .liveInstances = target->liveInstances, .builtIn = target->builtIn};
}

bool MaterialTargetCatalog::supported(std::string_view id) const { return find(id).has_value(); }

void MaterialTargetCatalog::unregister(std::uint64_t token) noexcept {
  const auto registration = std::ranges::find(m_tokens, token, [](const auto& item) {
    return item.first;
  });
  if (registration == m_tokens.end()) return;
  const std::string id = registration->second;
  m_tokens.erase(registration);
  const auto target = std::ranges::find(m_targets, id, [](const State& state) {
    return state.descriptor.id;
  });
  if (target == m_targets.end() || target->liveInstances == 0) return;
  --target->liveInstances;
  if (!target->builtIn && target->liveInstances == 0) m_targets.erase(target);
  m_changed.emit();
}

std::vector<std::string> materialTargetSurfacePath(std::string_view baseSurface, std::string_view targetId) {
  std::vector<std::string> result;
  const auto append = [&result](std::string_view value) {
    if (!value.empty() && (result.empty() || result.back() != value)) result.emplace_back(value);
  };
  append(baseSurface);
  if (const auto target = MaterialTargetCatalog::instance().find(targetId)) {
    for (const auto& surface : target->descriptor.surfaces) append(surface);
  }
  return result;
}

} // namespace Style
