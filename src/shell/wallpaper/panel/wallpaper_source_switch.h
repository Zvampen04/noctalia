#pragma once

#include "core/deferred_call.h"
#include "i18n/i18n.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/style.h"

#include <memory>
#include <string>

namespace wallpaper {

  inline constexpr const char* kWallhavenPanel = "noctalia/wallhaven:browser";

  // Both views share navigation; defer replacement until the input callback
  // has returned because opening the other panel destroys this control.
  inline std::unique_ptr<Flex> sourceSwitch(bool online, float scale) {
    return ui::row(
        {.gap = Style::spaceSm * scale},
        ui::segmented({
            .options =
                std::vector<ui::SegmentedOption>{
                    {.label = i18n::tr("wallpaper.panel.title"), .glyph = "folder"},
                    {.label = "Wallhaven", .glyph = "search"},
                },
            .selectedIndex = online ? 1U : 0U,
            .scale = scale,
            .compact = true,
            .onChange = [online](std::size_t index) {
              if ((index == 1) == online)
                return;
              DeferredCall::callLater([index] {
                auto& manager = PanelManager::instance();
                const std::string bar(manager.attachedSourceBarName());
                manager.openPanel(
                    index == 1 ? kWallhavenPanel : "wallpaper",
                    {
                        .output = manager.attachedPanelOutput(),
                        .sourceBarName = bar,
                    }
                );
              });
            },
        })
    );
  }

} // namespace wallpaper
