#include "shell/settings/slider_preview.h"
#include <cassert>
#include <array>
#include <thread>
#include <vector>

int main() {
  std::vector<double> applied;
  auto preview = std::make_shared<settings::SliderPreview>([&](double value) { applied.push_back(value); });
  for (int i = 0; i < 1000; ++i) preview->queue(i);
  assert(applied.empty());
  std::this_thread::sleep_for(std::chrono::milliseconds(90));
  TimerManager::instance().tick();
  assert((applied == std::vector<double>{999}));
  preview->queue(1000);
  preview->finish(1001);
  preview->finish(1001);
  std::this_thread::sleep_for(std::chrono::milliseconds(90));
  TimerManager::instance().tick();
  assert((applied == std::vector<double>{999,1001}));
  preview->queue(1002);
  preview.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(90));
  TimerManager::instance().tick();
  assert(applied.size() == 2);
  using Curve = std::array<float, 4>;
  const Curve original{0.2F, 0.8F, 0.4F, 1.F};
  std::vector<Curve> curves;
  auto curve = std::make_shared<settings::SettingPreview<Curve>>([&](Curve value) { curves.push_back(value); });
  for (int i = 0; i < 1000; ++i) curve->queue(Curve{float(i) / 1000.F, 0.2F, 0.7F, 1.F});
  curve->finish(original); // Escape restores the original curve, including any pending drag.
  std::this_thread::sleep_for(std::chrono::milliseconds(90));
  TimerManager::instance().tick();
  assert((curves == std::vector<Curve>{original}));
}
