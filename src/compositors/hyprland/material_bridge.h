#pragma once
#include <memory>
struct Config;
namespace compositors::hyprland {
class MaterialBridge {
public:
  MaterialBridge();
  ~MaterialBridge();
  void update(const Config& config);
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}
