#include "shell/settings/style_gallery.h"

#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace settings {
  namespace {
    using Json = nlohmann::json;

    constexpr std::size_t kMaxDocumentBytes = 256 * 1024;
    constexpr std::size_t kMaxEntries = 64;
    constexpr std::size_t kMaxNameBytes = 96;
    constexpr std::size_t kMaxDescriptionBytes = 512;
    constexpr std::size_t kMaxReferences = 8;

    std::string stringField(const Json& object, const char* key, std::size_t maxBytes, bool allowEmpty = false) {
      const auto it = object.find(key);
      if (it == object.end() || !it->is_string()) throw std::invalid_argument(std::string("Missing string: ") + key);
      std::string result = it->get<std::string>();
      if ((!allowEmpty && result.empty()) || result.size() > maxBytes)
        throw std::invalid_argument(std::string("Invalid string: ") + key);
      return result;
    }

    float numberField(const Json& object, const char* key, float maximum) {
      const auto it = object.find(key);
      if (it == object.end() || !it->is_number()) throw std::invalid_argument(std::string("Missing number: ") + key);
      const double value = it->get<double>();
      if (!std::isfinite(value) || value < 0.0 || value > maximum)
        throw std::invalid_argument(std::string("Invalid number: ") + key);
      return static_cast<float>(value);
    }

    bool oneOf(std::string_view value, std::initializer_list<std::string_view> accepted) {
      for (const auto candidate : accepted)
        if (value == candidate) return true;
      return false;
    }
  } // namespace

  StyleGallery parseStyleGallery(std::string_view document) {
    StyleGallery result;
    try {
      if (document.size() > kMaxDocumentBytes) throw std::invalid_argument("Style gallery metadata is too large");
      const Json root = Json::parse(document);
      const auto version = root.is_object() ? root.find("version") : root.end();
      if (!root.is_object() || root.size() != 2 || !root.contains("entries") || version == root.end() ||
          !version->is_number_integer() || version->get<int>() != 1)
        throw std::invalid_argument("Unsupported style gallery version");
      const auto entries = root.find("entries");
      if (entries == root.end() || !entries->is_array() || entries->size() > kMaxEntries)
        throw std::invalid_argument("Invalid style gallery entries");
      std::set<std::string> names;
      for (const auto& item : *entries) {
        if (!item.is_object()) throw std::invalid_argument("Invalid style gallery entry");
        StyleGalleryEntry entry;
        entry.name = stringField(item, "name", kMaxNameBytes);
        entry.category = stringField(item, "category", 24);
        entry.description = stringField(item, "description", kMaxDescriptionBytes, true);
        if (!oneOf(entry.category, {"factory", "reference", "saved"}))
          throw std::invalid_argument("Invalid style gallery category");
        const std::set<std::string> allowedFields = entry.category == "reference"
            ? std::set<std::string>{"name", "category", "description", "preview", "references"}
            : std::set<std::string>{"name", "category", "description", "preview"};
        for (const auto& [key, value] : item.items()) {
          (void)value;
          if (!allowedFields.contains(key)) throw std::invalid_argument("Unknown style gallery entry field");
        }
        if (!names.insert(entry.name).second) throw std::invalid_argument("Duplicate style gallery name");

        const auto preview = item.find("preview");
        if (preview == item.end() || !preview->is_object() || preview->size() != 9)
          throw std::invalid_argument("Invalid style gallery preview fields");
        entry.preview.position = stringField(*preview, "position", 8);
        entry.preview.barStyle = stringField(*preview, "bar_style", 16);
        entry.preview.radius = numberField(*preview, "radius", 512.0F);
        entry.preview.border = numberField(*preview, "border", 32.0F);
        entry.preview.material = stringField(*preview, "material", 48);
        entry.preview.layout = stringField(*preview, "layout", 48);
        const auto motion = preview->find("motion");
        if (motion == preview->end() || !motion->is_boolean()) throw std::invalid_argument("Missing boolean: motion");
        entry.preview.motion = motion->get<bool>();
        entry.preview.font = stringField(*preview, "font", 128, true);
        entry.preview.cursor = stringField(*preview, "cursor", 128, true);
        if (!oneOf(entry.preview.position, {"top", "right", "bottom", "left"}) ||
            !oneOf(entry.preview.barStyle, {"islands", "full", "fit", "dock", "notch"}))
          throw std::invalid_argument("Invalid style gallery bar preview");
        if (entry.category == "reference") {
          const auto references = item.find("references");
          if (references == item.end() || !references->is_array() || references->empty() ||
              references->size() > kMaxReferences)
            throw std::invalid_argument("Invalid style gallery references");
          std::set<std::string> ids, sources;
          for (const auto& value : *references) {
            if (!value.is_object() || value.size() != 4 || !value.contains("id") || !value.contains("source") ||
                !value.contains("dimensions") || !value.contains("state"))
              throw std::invalid_argument("Invalid style gallery reference");
            StyleGalleryEntry::Reference reference;
            reference.id = stringField(value, "id", 96);
            reference.source = stringField(value, "source", 192);
            reference.state = stringField(value, "state", 96);
            if (reference.source.find('/') != std::string::npos || reference.source.find('\\') != std::string::npos)
              throw std::invalid_argument("Reference source must be a basename");
            const auto dimensions = value.find("dimensions");
            if (dimensions == value.end() || !dimensions->is_array() || dimensions->size() != 2 ||
                !(*dimensions)[0].is_number_integer() || !(*dimensions)[1].is_number_integer())
              throw std::invalid_argument("Invalid reference dimensions");
            reference.width = (*dimensions)[0].get<int>();
            reference.height = (*dimensions)[1].get<int>();
            if (reference.width <= 0 || reference.height <= 0 || reference.width > 32768 || reference.height > 32768 ||
                !ids.insert(reference.id).second || !sources.insert(reference.source).second)
              throw std::invalid_argument("Invalid or duplicate style gallery reference");
            entry.references.push_back(std::move(reference));
          }
        }
        result.entries.push_back(std::move(entry));
      }
    } catch (const std::exception& error) {
      result.entries.clear();
      result.error = error.what();
    }
    return result;
  }

} // namespace settings
