#include "shell/settings/compact_layout_editor.h"

#include "render/scene/input_area.h"
#include "shell/control_center/compact_layout.h"
#include "shell/settings/settings_content.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"

#include <cmath>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace settings {
  namespace {
    using compact_layout::Cell;
    struct Model {
      int columns = 6;
      std::vector<Cell> cells;
      std::vector<std::vector<Cell>> history;
      std::string selected;
      bool dragging = false, resizing = false;
      float startX = 0, startY = 0;
      Cell original;
      std::function<void()> onChange;
      void changed() {
        if (onChange)
          onChange();
      }
      void checkpoint() {
        changed();
        history.push_back(cells);
        if (history.size() > 40)
          history.erase(history.begin());
      }
    };
    class Canvas final : public Flex {
    public:
      Canvas(std::shared_ptr<Model> model, float scale) : m(std::move(model)), s(scale) {
        m->onChange = [this] { markLayoutDirty(); };
        setFillWidth(true);
        setMinHeight(432 * s);
        setMaxHeight(432 * s);
        setFill(colorSpecFromRole(ColorRole::SurfaceVariant));
        setRadius(12 * s);
        for (auto kind : compact_layout::kinds) {
          auto box = ui::column(
              {.align = FlexAlign::Center,
               .justify = FlexJustify::Center,
               .fill = colorSpecFromRole(ColorRole::Surface),
               .radius = 10 * s}
          );
          auto* ptr = box.get();
          box->setParticipatesInLayout(false);
          box->addChild(ui::label({.text = std::string(kind) + "  ↘", .fontSize = 12 * s}));
          auto area = std::make_unique<InputArea>();
          auto* input = area.get();
          input->setFocusable(true);
          input->setTabFocusKey("compact-layout-" + std::string(kind));
          input->setRetainsFocusOnPointerRelease(true);
          input->setParticipatesInLayout(false);
          input->setOnPress([this, kind = std::string(kind), ptr](const InputArea::PointerData& p) {
            const auto it = std::ranges::find(m->cells, kind, &Cell::kind);
            if (it == m->cells.end() || p.button != BTN_LEFT)
              return;
            if (p.pressed) {
              m->selected = kind;
              m->checkpoint();
              m->original = *it;
              m->startX = p.sceneX;
              m->startY = p.sceneY;
              m->dragging = true;
              m->resizing = p.localX > ptr->width() - 22 * s && p.localY > ptr->height() - 22 * s;
            } else
              m->dragging = false;
          });
          input->setOnMotion([this, kind = std::string(kind)](const InputArea::PointerData& p) {
            if (!m->dragging || m->selected != kind)
              return;
            Cell next = m->original;
            const int dx = std::lround((p.sceneX - m->startX) / std::max(1.F, width() / m->columns));
            const int dy = std::lround((p.sceneY - m->startY) / (36 * s));
            if (m->resizing) {
              next.w += dx;
              next.h += dy;
            } else {
              next.x += dx;
              next.y += dy;
            }
            if (compact_layout::fits(m->cells, next, m->columns)) {
              *std::ranges::find(m->cells, kind, &Cell::kind) = next;
              m->changed();
            }
          });
          input->setOnCancel([this] {
            if (m->dragging) {
              auto it = std::ranges::find(m->cells, m->original.kind, &Cell::kind);
              if (it != m->cells.end())
                *it = m->original;
            }
            m->dragging = false;
            m->changed();
          });
          input->setOnKeyDown([this, kind = std::string(kind)](const InputArea::KeyData& key) {
            if (!key.pressed)
              return;
            auto it = std::ranges::find(m->cells, kind, &Cell::kind);
            if (it == m->cells.end())
              return;
            m->selected = kind;
            Cell next = *it;
            switch (key.sym) {
            case XKB_KEY_Left:
              --next.x;
              break;
            case XKB_KEY_Right:
              ++next.x;
              break;
            case XKB_KEY_Up:
              --next.y;
              break;
            case XKB_KEY_Down:
              ++next.y;
              break;
            case XKB_KEY_bracketleft:
              --next.w;
              break;
            case XKB_KEY_bracketright:
              ++next.w;
              break;
            case XKB_KEY_Page_Up:
              --next.h;
              break;
            case XKB_KEY_Page_Down:
              ++next.h;
              break;
            case XKB_KEY_Delete:
              m->checkpoint();
              m->cells.erase(it);
              return;
            default:
              return;
            }
            if (compact_layout::fits(m->cells, next, m->columns)) {
              m->checkpoint();
              *it = next;
            }
          });
          box->addChild(std::move(area));
          items.push_back({std::string(kind), ptr, input});
          addChild(std::move(box));
        }
      }
      ~Canvas() override { m->onChange = {}; }

    protected:
      LayoutSize doMeasure(Renderer& renderer, const LayoutConstraints& constraints) override {
        return measureByLayout(renderer, constraints);
      }
      void doArrange(Renderer& renderer, const LayoutRect& rect) override {
        arrangeByLayout(renderer, rect);
      }
      void doLayout(Renderer& renderer) override {
        Flex::doLayout(renderer);
        for (auto& item : items) {
          auto it = std::ranges::find(m->cells, item.kind, &Cell::kind);
          item.box->setVisible(it != m->cells.end());
          if (it == m->cells.end())
            continue;
          const float unit = width() / m->columns;
          item.box->setPosition(it->x * unit + 3 * s, it->y * 36 * s + 3 * s);
          item.box->setFrameSize(it->w * unit - 6 * s, it->h * 36 * s - 6 * s);
          item.box->setBorder(colorSpecFromRole(m->selected == item.kind ? ColorRole::Primary : ColorRole::Outline), s);
          item.box->layout(renderer);
          item.input->setSize(item.box->width(), item.box->height());
        }
      }

    private:
      struct Item {
        std::string kind;
        Flex* box;
        InputArea* input;
      };
      std::shared_ptr<Model> m;
      float s;
      std::vector<Item> items;
    };
  } // namespace
  std::unique_ptr<Flex> makeCompactLayoutEditor(const SettingsContentContext& ctx) {
    auto model = std::make_shared<Model>();
    model->columns = ctx.config.controlCenter.compactColumns;
    model->cells = compact_layout::parse(ctx.config.controlCenter.compactLayout, model->columns);
    auto root = ui::column({.gap = 8 * ctx.scale, .fillWidth = true});
    root->addChild(ui::label({.text = "Quick Settings layout", .fontSize = 18 * ctx.scale}));
    root->addChild(
        ui::label(
            {.text = "Drag to move; bottom-right corner to resize. Arrows move, [ / ] change width, Page Up/Down "
                     "change height, Delete removes. Apply saves; Undo reverses preview edits.",
             .fontSize = 12 * ctx.scale,
             .maxLines = 3}
        )
    );
    auto actions = ui::row({.wrap = true, .gap = 6 * ctx.scale});
    actions->addChild(ui::button({.text = "Apply", .onClick = [model, save = ctx.setOverrides] {
                                    save(
                                        {{{"control_center", "compact_layout"}, compact_layout::encode(model->cells)},
                                         {{"control_center", "compact_sections"}, true}}
                                    );
                                  }}));
    actions->addChild(ui::button({.text = "Undo", .onClick = [model] {
                                    if (!model->history.empty()) {
                                      model->cells = model->history.back();
                                      model->history.pop_back();
                                      model->changed();
                                    }
                                  }}));
    actions->addChild(ui::button({.text = "Tidy", .onClick = [model] {
                                    model->checkpoint();
                                    model->cells = compact_layout::tidy(model->cells, model->columns);
                                  }}));
    actions->addChild(ui::button({.text = "Reset preview", .onClick = [model] {
                                    model->checkpoint();
                                    model->cells = compact_layout::defaults(model->columns);
                                  }}));
    actions->addChild(ui::button({.text = "Use automatic layout", .onClick = [clear = ctx.clearOverride] {
                                    clear({"control_center", "compact_layout"});
                                  }}));
    actions->addChild(ui::button({.text = "Remove selected", .onClick = [model] {
                                    model->checkpoint();
                                    std::erase_if(model->cells, [&](const Cell& c) {
                                      return c.kind == model->selected;
                                    });
                                  }}));
    root->addChild(std::move(actions));
    root->addChild(std::make_unique<Canvas>(model, ctx.scale));
    auto add = ui::row({.wrap = true, .gap = 6 * ctx.scale});
    for (auto kind : compact_layout::kinds)
      add->addChild(
          ui::button({.text = "+ " + std::string(kind), .onClick = [model, kind = std::string(kind)] {
                        if (std::ranges::any_of(model->cells, [&](const Cell& c) { return c.kind == kind; }))
                          return;
                        Cell c{kind, 0, 0, model->columns, kind == "media" ? 3 : kind == "notifications" ? 2 : 1};
                        for (int y = 0; y + c.h <= 12; ++y) {
                          c.y = y;
                          if (compact_layout::fits(model->cells, c, model->columns)) {
                            model->checkpoint();
                            model->cells.push_back(c);
                            model->selected = kind;
                            return;
                          }
                        }
                      }})
      );
    root->addChild(std::move(add));
    root->addChild(
        ui::label(
            {.text = "Brightness and media still respect device availability and your media visibility setting. "
                     "Utility buttons remain configurable in Shortcuts.",
             .fontSize = 12 * ctx.scale,
             .maxLines = 3}
        )
    );
    return root;
  }
} // namespace settings
