#pragma once

#include "shell/settings/settings_registry.h"
#include "shell/bar/bar_corner_shape.h"

namespace settings {

// The named shapes are editing recipes for public bar properties. Rendering
// reads those properties, so every recipe remains independently adjustable.
inline SelectSetting barPresentationSetting(const BarConfig& bar, std::vector<std::string> basePath) {
  SelectSetting setting;
  setting.options = {{.value="islands", .label="Islands — Split pills"},
                     {.value="full", .label="Full — Edge to edge"},
                     {.value="fit", .label="Fit — Inset rounded frame"},
                     {.value="dock", .label="Dock — Direct edge junction"},
                     {.value="notch", .label="Notch — Flowing shoulders"},
                     {.value="custom", .label="Custom"}};
  const auto inner = barInnerEdgeCorners(bar.position);
  const bool straightJunction = (inner.topLeft || bar.radiusTopLeft == 0)
      && (inner.topRight || bar.radiusTopRight == 0)
      && (inner.bottomLeft || bar.radiusBottomLeft == 0)
      && (inner.bottomRight || bar.radiusBottomRight == 0);
  const bool square = bar.radiusTopLeft == 0 && bar.radiusTopRight == 0
      && bar.radiusBottomLeft == 0 && bar.radiusBottomRight == 0;
  setting.selectedValue = bar.sectionBackgrounds ? "islands"
      : bar.marginEdge > 0 && bar.marginEnds > 0 && !bar.concaveEdgeCorners ? "fit"
      : bar.marginEdge == 0 && bar.marginEnds == 0 && bar.maxLength == 0 && !bar.concaveEdgeCorners && square ? "full"
      : bar.marginEdge == 0 && bar.marginEnds > 0 && bar.concaveEdgeCorners ? "notch"
      : bar.marginEdge == 0 && bar.marginEnds > 0 && !bar.concaveEdgeCorners && straightJunction ? "dock"
      : "custom";
  for (const auto* key : {"max_length", "margin_edge", "margin_ends", "concave_edge_corners", "radius",
                          "radius_top_left", "radius_top_right", "radius_bottom_left", "radius_bottom_right"}) {
    auto path = basePath;
    path.emplace_back(key);
    setting.linkedPaths.push_back(std::move(path));
  }
  setting.groupedCommit = [bar, inner, basePath = std::move(basePath)](
      std::string_view selected, const std::vector<std::string>& primaryPath) {
    std::vector<std::pair<std::vector<std::string>, ConfigOverrideValue>> changes;
    if (selected != "islands" && selected != "full" && selected != "fit"
        && selected != "dock" && selected != "notch") return changes;
    auto add = [&](const char* key, ConfigOverrideValue value) {
      auto path = basePath;
      path.emplace_back(key);
      changes.emplace_back(std::move(path), std::move(value));
    };
    const bool inset = selected == "islands" || selected == "fit";
    const bool full = selected == "full";
    const bool dock = selected == "dock";
    const auto radius = static_cast<std::int64_t>(std::max(0, bar.radius));
    changes.emplace_back(primaryPath, selected == "islands");
    if (full) add("max_length", std::int64_t(0));
    add("margin_edge", std::int64_t(inset ? std::max(8, bar.marginEdge) : 0));
    add("margin_ends", std::int64_t(full ? 0 : std::max(8, bar.marginEnds)));
    add("concave_edge_corners", selected == "notch");
    // Keep the editable radius seed even in Full so returning to another shape
    // restores its size. Explicit corner values determine the painted geometry.
    add("radius", radius);
    add("radius_top_left", full || (dock && !inner.topLeft) ? std::int64_t(0) : radius);
    add("radius_top_right", full || (dock && !inner.topRight) ? std::int64_t(0) : radius);
    add("radius_bottom_left", full || (dock && !inner.bottomLeft) ? std::int64_t(0) : radius);
    add("radius_bottom_right", full || (dock && !inner.bottomRight) ? std::int64_t(0) : radius);
    return changes;
  };
  return setting;
}

} // namespace settings
