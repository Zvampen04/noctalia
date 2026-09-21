#include "shell/settings/slider_preview.h"
#include <cassert>
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
}
