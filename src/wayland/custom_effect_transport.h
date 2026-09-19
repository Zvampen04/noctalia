#pragma once

#include "material/custom_effect_resource.h"
#include "render/custom_effect/custom_effect_types.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

struct noctalia_material_effect_v1;
struct noctalia_material_manager_v1;

class CustomEffectTransportRegistry {
public:
  enum class State { Missing, Importing, Accepted, Ready, Rejected };

  CustomEffectTransportRegistry();
  ~CustomEffectTransportRegistry();
  CustomEffectTransportRegistry(const CustomEffectTransportRegistry&) = delete;
  CustomEffectTransportRegistry& operator=(const CustomEffectTransportRegistry&) = delete;

  void setManager(noctalia_material_manager_v1* manager);
  [[nodiscard]] noctalia::material::CustomEffectTransportDigest
  ensure(const std::shared_ptr<const CustomEffectAsset>& asset);
  [[nodiscard]] bool retain(const std::shared_ptr<const CustomEffectAsset>& asset);
  void release(const noctalia::material::CustomEffectTransportDigest& digest);
  [[nodiscard]] State state(const noctalia::material::CustomEffectTransportDigest& digest) const;
  [[nodiscard]] std::string diagnostic(const noctalia::material::CustomEffectTransportDigest& digest) const;
  [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }
  [[nodiscard]] std::uint64_t managerGeneration() const noexcept { return m_managerGeneration; }

  [[nodiscard]] std::uint64_t subscribe(std::function<void()> callback);
  void unsubscribe(std::uint64_t token);
  void reset();

private:
  struct Entry;
  using Digest = noctalia::material::CustomEffectTransportDigest;

  void notify();
  static void accepted(void* data, noctalia_material_effect_v1* effect);
  static void ready(void* data, noctalia_material_effect_v1* effect);
  static void rejected(
      void* data, noctalia_material_effect_v1* effect, std::uint32_t reason, const char* diagnostic);

  noctalia_material_manager_v1* m_manager = nullptr;
  std::map<Digest, std::unique_ptr<Entry>> m_entries;
  std::map<std::uint64_t, std::function<void()>> m_subscribers;
  std::uint64_t m_nextSubscriber = 1;
  std::uint64_t m_generation = 1;
  std::uint64_t m_managerGeneration = 1;
};
