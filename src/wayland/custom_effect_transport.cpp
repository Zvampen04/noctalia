#include "wayland/custom_effect_transport.h"

#include "material/scene_transport.h"
#include "noctalia-material-v1-client-protocol.h"

#include <cerrno>
#include <fcntl.h>
#include <span>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-util.h>

namespace {
using Digest = noctalia::material::CustomEffectTransportDigest;

Digest transportDigest(const CustomEffectAsset& asset) {
  return noctalia::material::customEffectTransportDigest(
      asset.abi, std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(asset.source.data()), asset.source.size()));
}

int sealedSource(std::string_view source) {
  const int fd = ::memfd_create("noctalia-custom-effect", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return -1;
  std::size_t offset = 0;
  while (offset < source.size()) {
    const auto count = ::write(fd, source.data() + offset, source.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ::close(fd); return -1; }
    offset += static_cast<std::size_t>(count);
  }
  if (::fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0
      || ::lseek(fd, 0, SEEK_SET) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
} // namespace

struct CustomEffectTransportRegistry::Entry {
  CustomEffectTransportRegistry* owner = nullptr;
  noctalia_material_effect_v1* resource = nullptr;
  State state = State::Missing;
  std::string diagnostic;
  std::size_t references = 0;
};

CustomEffectTransportRegistry::CustomEffectTransportRegistry() = default;

CustomEffectTransportRegistry::~CustomEffectTransportRegistry() { reset(); }

void CustomEffectTransportRegistry::setManager(noctalia_material_manager_v1* manager) {
  if (m_manager == manager) return;
  reset();
  m_manager = manager;
}

CustomEffectTransportRegistry::Digest
CustomEffectTransportRegistry::ensure(const std::shared_ptr<const CustomEffectAsset>& asset) {
  if (!asset) return {};
  const auto digest = transportDigest(*asset);
  if (m_entries.contains(digest) || m_manager == nullptr
      || noctalia_material_manager_v1_get_version(m_manager) < noctalia::material::kCustomEffectTransportVersion)
    return digest;

  auto entry = std::make_unique<Entry>();
  entry->owner = this;
  const int fd = sealedSource(asset->source);
  if (fd < 0) {
    entry->state = State::Rejected;
    entry->diagnostic = "Cannot create a sealed custom effect transport";
    m_entries.emplace(digest, std::move(entry));
    ++m_generation;
    notify();
    return digest;
  }
  wl_array wireDigest{digest.size(), 0, const_cast<std::uint8_t*>(digest.data())};
  entry->resource = noctalia_material_manager_v1_import_effect(
      m_manager, &wireDigest, asset->abi, wl_fixed_from_int(256), fd,
      static_cast<std::uint32_t>(asset->source.size()));
  ::close(fd);
  if (entry->resource == nullptr) {
    entry->state = State::Rejected;
    entry->diagnostic = "The compositor refused the custom effect transport object";
  } else {
    static const noctalia_material_effect_v1_listener listener{
        .accepted = &CustomEffectTransportRegistry::accepted,
        .ready = &CustomEffectTransportRegistry::ready,
        .rejected = &CustomEffectTransportRegistry::rejected,
    };
    entry->state = State::Importing;
    noctalia_material_effect_v1_add_listener(entry->resource, &listener, entry.get());
  }
  const bool rejected = entry->state == State::Rejected;
  m_entries.emplace(digest, std::move(entry));
  ++m_generation;
  if (rejected) notify();
  return digest;
}

bool CustomEffectTransportRegistry::retain(const std::shared_ptr<const CustomEffectAsset>& asset) {
  const auto digest = ensure(asset);
  const auto found = m_entries.find(digest);
  if (found == m_entries.end()) return false;
  ++found->second->references;
  return true;
}

void CustomEffectTransportRegistry::release(const Digest& digest) {
  const auto found = m_entries.find(digest);
  if (found == m_entries.end() || found->second->references == 0) return;
  if (--found->second->references != 0) return;
  if (found->second->resource != nullptr) noctalia_material_effect_v1_destroy(found->second->resource);
  m_entries.erase(found);
  ++m_generation;
}

CustomEffectTransportRegistry::State
CustomEffectTransportRegistry::state(const Digest& digest) const {
  if (const auto found = m_entries.find(digest); found != m_entries.end()) return found->second->state;
  return State::Missing;
}

std::string CustomEffectTransportRegistry::diagnostic(const Digest& digest) const {
  if (const auto found = m_entries.find(digest); found != m_entries.end()) return found->second->diagnostic;
  return {};
}

std::uint64_t CustomEffectTransportRegistry::subscribe(std::function<void()> callback) {
  const auto token = m_nextSubscriber++;
  m_subscribers.emplace(token, std::move(callback));
  return token;
}

void CustomEffectTransportRegistry::unsubscribe(std::uint64_t token) { m_subscribers.erase(token); }

void CustomEffectTransportRegistry::reset() {
  const bool changed = m_manager != nullptr || !m_entries.empty();
  for (auto& [digest, entry] : m_entries) {
    (void)digest;
    if (entry->resource != nullptr) noctalia_material_effect_v1_destroy(entry->resource);
  }
  m_entries.clear();
  m_manager = nullptr;
  ++m_generation;
  ++m_managerGeneration;
  if (changed) notify();
}

void CustomEffectTransportRegistry::notify() {
  const auto subscribers = m_subscribers;
  for (const auto& [token, callback] : subscribers) {
    (void)token;
    if (callback) callback();
  }
}

void CustomEffectTransportRegistry::accepted(void* data, noctalia_material_effect_v1*) {
  auto& entry = *static_cast<Entry*>(data);
  if (entry.state != State::Importing) return;
  entry.state = State::Accepted;
  ++entry.owner->m_generation;
  entry.owner->notify();
}

void CustomEffectTransportRegistry::ready(void* data, noctalia_material_effect_v1*) {
  auto& entry = *static_cast<Entry*>(data);
  if (entry.state == State::Rejected) return;
  entry.state = State::Ready;
  entry.diagnostic.clear();
  ++entry.owner->m_generation;
  entry.owner->notify();
}

void CustomEffectTransportRegistry::rejected(
    void* data, noctalia_material_effect_v1*, std::uint32_t, const char* diagnostic) {
  auto& entry = *static_cast<Entry*>(data);
  entry.state = State::Rejected;
  entry.diagnostic = diagnostic != nullptr ? diagnostic : "The compositor rejected the custom effect";
  ++entry.owner->m_generation;
  entry.owner->notify();
}
