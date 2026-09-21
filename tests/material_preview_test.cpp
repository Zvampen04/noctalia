#include "config/config_service.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
  namespace fs = std::filesystem;
  std::string pattern = (fs::temp_directory_path() / "material-preview-XXXXXX").string();
  const char* directory = mkdtemp(pattern.data());
  assert(directory);
  const fs::path root(directory);
  for (const auto* key : {"NOCTALIA_CONFIG_HOME", "NOCTALIA_STATE_HOME", "NOCTALIA_DATA_HOME",
                         "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) {
    const auto path = root / key;
    fs::create_directories(path / "noctalia");
    setenv(key, path.c_str(), 1);
  }
  const auto settings = root / "NOCTALIA_STATE_HOME/noctalia/settings.toml";
  std::ofstream(settings) << "[shell.material_overrides.surfaces.\"window.background\"]\nthickness = 14.0\n";
  std::ofstream(root / "NOCTALIA_CONFIG_HOME/noctalia/config.toml") << "\n";
  {
    ConfigService config;
    const auto original = config.config().shell.materialOverrides;
    const std::vector<std::string> path{"shell", "material_overrides", "surfaces", "window.background", "thickness"};
    int previews = 0;
    config.addReloadCallback([&] { if (config.materialPreviewUpdate()) ++previews; });
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) assert(config.setOverride(path, double(20 + i % 20)));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    assert(previews == 100);
    assert(config.config().shell.materialOverrides.surfaces.at("window.background").thickness == 39.F);
    assert(toml::parse_file(settings.string())["shell"]["material_overrides"]["surfaces"]["window.background"]["thickness"].value<double>() == 14.0);
    const auto beforeInvalid = config.config().shell.materialOverrides;
    assert(!config.setOverride(path, -100.0));
    assert(config.config().shell.materialOverrides == beforeInvalid);
    config.cancelProfilePreview();
    assert(config.config().shell.materialOverrides == original);
    assert(config.setOverride(path, 30.0));
    assert(config.commitProfilePreview());
    assert(toml::parse_file(settings.string())["shell"]["material_overrides"]["surfaces"]["window.background"]["thickness"].value<double>() == 30.0);
    std::cout << "100 material previews: " << elapsed.count() << " ms\n";
  }
  fs::remove_all(root);
}
