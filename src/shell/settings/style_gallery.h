#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace settings {

  struct StyleGalleryPreview {
    std::string position;
    std::string barStyle;
    float radius = 0.0F;
    float border = 0.0F;
    std::string material;
    std::string layout;
    bool motion = true;
    std::string font;
    std::string cursor;
  };

  struct StyleGalleryEntry {
    struct Reference {
      std::string id;
      std::string source;
      int width = 0;
      int height = 0;
      std::string state;
    };
    std::string name;
    std::string category;
    std::string description;
    StyleGalleryPreview preview;
    std::vector<Reference> references;
  };

  struct StyleGallery {
    std::vector<StyleGalleryEntry> entries;
    std::string error;
  };

  // Parses the bounded v1 metadata emitted by the integrated settings backend.
  // The payload is descriptive only; preset application continues through the
  // acknowledged SettingsWindow profile transaction.
  [[nodiscard]] StyleGallery parseStyleGallery(std::string_view document);

} // namespace settings
