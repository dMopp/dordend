#include "rummage/views/tab_bar.hpp"

#include <algorithm>
#include <utility>

namespace rummage::views {
namespace {

constexpr float kPadding = 8.0F;     // before the first tab
constexpr float kGap = 2.0F;         // between tabs
constexpr float kTop = 5.0F;         // above the tabs: they stand on the bar's bottom line
constexpr float kInset = 12.0F;      // before a tab's title
constexpr float kCloseSize = 22.0F;  // the ×'s hover box
constexpr float kCloseIcon = 14.0F;
constexpr float kAddSize = 26.0F;
constexpr float kTextSize = 13.0F;

bool inside(const ui::Rect& rect, float x, float y) {
    return !rect.empty() && x >= rect.x && x < rect.right() && y >= rect.y && y < rect.bottom();
}

}  // namespace

TabBar::TabBar(std::function<void()> redraw, Handlers handlers)
    : redraw_{std::move(redraw)}, handlers_{std::move(handlers)} {}

void TabBar::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                  ui::IconSet& icons, const std::vector<std::string>& titles, std::size_t active) {
    bounds_ = bounds;
    tabs_.clear();
    closes_.clear();
    if (hovered_ && hovered_->part != Part::add && hovered_->index >= titles.size()) {
        hovered_.reset();  // that tab went away
    }
    renderer.fill_background(bounds, palette.surface_container_low);

    const float room = std::max(0.0F, bounds.width - (2.0F * kPadding) - kAddSize - kGap);
    const float count = static_cast<float>(std::max<std::size_t>(titles.size(), 1));
    const float width = std::clamp((room - (kGap * (count - 1.0F))) / count, kMinWidth, kMaxWidth);
    const float top = bounds.y + kTop;
    const float height = bounds.height - kTop;
    float x = bounds.x + kPadding;
    for (std::size_t i = 0; i < titles.size(); ++i) {
        const ui::Rect tab{x, top, width, height};
        const ui::Rect close{tab.right() - kCloseSize - 6.0F, top + ((height - kCloseSize) / 2.0F),
                             kCloseSize, kCloseSize};
        tabs_.push_back(tab);
        closes_.push_back(close);
        const bool shown = i == active;
        const bool over = hovered_ && hovered_->index == i && hovered_->part != Part::add;
        if (shown) {
            renderer.draw_box(tab, {.fill = palette.surface,
                                    .radius = {},
                                    .border_width = 1.0F,
                                    .border = palette.outline_variant});
            // The tab opens onto the view below: no line under it.
            renderer.fill_rect({tab.x + 1.0F, bounds.bottom() - 1.0F, tab.width - 2.0F, 1.0F},
                               palette.surface);
        } else if (over) {
            renderer.draw_box(tab, {.fill = palette.surface_container, .radius = {}});
        }
        const ui::Color text = shown || over ? palette.on_surface : palette.on_surface_variant;
        renderer.draw_text(titles[i], tab.x + kInset, top + ((height - kTextSize) / 2.0F) - 2.0F,
                           kTextSize, text, std::max(1.0F, close.x - (tab.x + kInset) - 4.0F));
        if (shown || over) {
            if (hovered_ == Hit{.part = Part::close, .index = i}) {
                renderer.draw_box(close, {.fill = palette.surface_container_highest, .radius = {}});
            }
            icons.draw(renderer, "x", close.x + ((kCloseSize - kCloseIcon) / 2.0F),
                       close.y + ((kCloseSize - kCloseIcon) / 2.0F), kCloseIcon,
                       palette.on_surface_variant);
        }
        x += width + kGap;
    }

    // The +: two bars, as the icon font has no plus.
    add_ = {x + kGap, top + ((height - kAddSize) / 2.0F), kAddSize, kAddSize};
    if (hovered_ && hovered_->part == Part::add) {
        renderer.draw_box(add_, {.fill = palette.surface_container, .radius = {}});
    }
    const float arm = 12.0F;
    const float thick = 1.5F;
    const float cx = add_.x + (kAddSize / 2.0F);
    const float cy = add_.y + (kAddSize / 2.0F);
    renderer.fill_rect({cx - (arm / 2.0F), cy - (thick / 2.0F), arm, thick},
                       palette.on_surface_variant);
    renderer.fill_rect({cx - (thick / 2.0F), cy - (arm / 2.0F), thick, arm},
                       palette.on_surface_variant);

    // The bar's bottom line, broken under the tab shown (drawn over it above).
    const ui::Rect line{bounds.x, bounds.bottom() - 1.0F, bounds.width, 1.0F / renderer.scale()};
    if (active < tabs_.size()) {
        const ui::Rect& tab = tabs_[active];
        renderer.fill_rect({line.x, line.y, tab.x - line.x, line.height}, palette.outline_variant);
        renderer.fill_rect({tab.right(), line.y, line.right() - tab.right(), line.height},
                           palette.outline_variant);
    } else {
        renderer.fill_rect(line, palette.outline_variant);
    }
}

void TabBar::not_drawn() {
    bounds_ = {};
    tabs_.clear();
    closes_.clear();
    add_ = {};
    hovered_.reset();
}

bool TabBar::claims(float x, float y) const { return inside(bounds_, x, y); }

std::optional<TabBar::Hit> TabBar::hit(float x, float y) const {
    if (inside(add_, x, y)) {
        return Hit{.part = Part::add};
    }
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        if (inside(tabs_[i], x, y)) {
            return Hit{.part = inside(closes_[i], x, y) ? Part::close : Part::tab, .index = i};
        }
    }
    return std::nullopt;
}

bool TabBar::clickable_at(float x, float y) const { return hit(x, y).has_value(); }

void TabBar::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    std::optional<Hit> over;
    if (event.kind != Kind::leave) {
        over = hit(event.x, event.y);
    }
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || !over) {
        return;
    }
    if (over->part == Part::add) {
        if (event.button == ui::PointerButton::left && handlers_.add) {
            handlers_.add();
        }
        return;
    }
    const bool close = event.button == ui::PointerButton::middle ||
                       (event.button == ui::PointerButton::left && over->part == Part::close);
    if (close) {
        if (handlers_.close) {
            handlers_.close(over->index);
        }
    } else if (event.button == ui::PointerButton::left && handlers_.select) {
        handlers_.select(over->index);
    }
}

}  // namespace rummage::views
