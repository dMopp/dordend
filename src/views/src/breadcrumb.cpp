#include "rummage/views/breadcrumb.hpp"

#include <algorithm>
#include <utility>

#include "draw_helpers.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {

constexpr float kPadding = 16.0F;  // either end of the bar
constexpr float kInset = 6.0F;     // either side of a step, inside its hover box
constexpr float kIconSize = 16.0F;
constexpr float kChevronSize = 12.0F;
constexpr float kChevronGap = 2.0F;  // either side of a chevron
constexpr float kBoxHeight = 26.0F;
constexpr float kTextSize = 14.0F;
constexpr std::string_view kEllipsis = "…";

}  // namespace

std::vector<Crumb> crumbs_of(const std::filesystem::path& folder,
                             const std::filesystem::path& home) {
    if (fs::is_trash(folder)) {
        return {{.path = folder, .label = {}, .icon = "trash"},
                {.path = folder, .label = std::string{i18n::tr("Trash")}, .icon = {}}};
    }
    const std::filesystem::path normal = folder.lexically_normal();
    std::filesystem::path base{"/"};
    std::string_view icon = "server";
    if (!home.empty() && home != "/") {
        const std::filesystem::path relative = normal.lexically_relative(home.lexically_normal());
        if (!relative.empty() && *relative.begin() != "..") {
            base = home.lexically_normal();
            icon = "home";
        }
    }
    std::vector<Crumb> crumbs{{.path = base, .label = {}, .icon = icon}};
    std::filesystem::path path = base;
    for (const std::filesystem::path& part : normal.lexically_relative(base)) {
        if (part.empty() || part == ".") {
            continue;
        }
        path /= part;
        crumbs.push_back({.path = path, .label = part.string(), .icon = {}});
    }
    return crumbs;
}

Breadcrumb::Breadcrumb(std::function<void()> redraw, Go go, std::filesystem::path home)
    : redraw_{std::move(redraw)}, go_{std::move(go)}, home_{std::move(home)} {}

void Breadcrumb::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                      ui::IconSet& icons, const Folder& folder) {
    bounds_ = bounds;
    std::vector<Crumb> crumbs = crumbs_of(folder.path(), home_);
    if (crumbs != crumbs_) {
        crumbs_ = std::move(crumbs);
        hovered_.reset();
    }

    const float top = bounds.y + ((kTitleHeight - kBoxHeight) / 2.0F);

    // Every step's width, then which fit: the first, "…", and as many of the last as there is
    // room for (the last one always, cut in the middle when even it is too long).
    const auto width_of = [&](const Crumb& crumb) {
        return (crumb.label.empty() ? kIconSize : text_width(renderer, crumb.label, kTextSize)) +
               (2.0F * kInset);
    };
    const float separator = kChevronSize + (2.0F * kChevronGap);
    const float room = std::max(0.0F, bounds.width - (2.0F * kPadding));
    std::vector<float> widths;
    widths.reserve(crumbs_.size());
    float total = 0.0F;
    for (const Crumb& crumb : crumbs_) {
        widths.push_back(width_of(crumb));
        total += widths.back() + (widths.size() > 1 ? separator : 0.0F);
    }
    std::size_t first_tail = 1;  // crumbs [1, first_tail) are left out
    if (total > room && crumbs_.size() > 2) {
        const float ellipsis = text_width(renderer, kEllipsis, kTextSize) + (2.0F * kInset);
        float used = widths.front() + separator + ellipsis;
        first_tail = crumbs_.size() - 1;
        used += separator + widths.back();
        while (first_tail > 1 && used + separator + widths[first_tail - 1] <= room) {
            --first_tail;
            used += separator + widths[first_tail];
        }
    }

    shown_.clear();
    float x = bounds.x + kPadding - kInset;
    const auto chevron = [&] {
        icons.draw(renderer, "chevron-right", x + kChevronGap,
                   top + ((kBoxHeight - kChevronSize) / 2.0F), kChevronSize,
                   palette.on_surface_variant);
        x += separator;
    };
    for (std::size_t i = 0; i < crumbs_.size(); ++i) {
        if (i == 1 && first_tail > 1) {
            const float width = text_width(renderer, kEllipsis, kTextSize) + (2.0F * kInset);
            chevron();
            renderer.draw_text(kEllipsis, x + kInset, bounds.y + 12.0F, kTextSize,
                               palette.on_surface_variant);
            shown_.push_back({.crumb = std::nullopt, .rect = {x, top, width, kBoxHeight}});
            x += width;
            i = first_tail - 1;
            continue;
        }
        if (i > 0) {
            chevron();
        }
        const Crumb& crumb = crumbs_[i];
        const bool last = i + 1 == crumbs_.size();
        const float width =
            std::min(widths[i], std::max((2.0F * kInset) + 1.0F, bounds.x + kPadding + room - x));
        const ui::Rect box{x, top, width, kBoxHeight};
        const bool drop = drop_target_ == crumb.path;
        if (drop) {
            renderer.draw_box(box, {.fill = palette.primary_container,
                                    .radius = {},
                                    .border_width = 1.5F,
                                    .border = palette.primary});
        } else if (hovered_ == i && !last) {
            renderer.draw_box(box, {.fill = palette.surface_container_high, .radius = {}});
        }
        ui::Color color = last || hovered_ == i ? palette.on_surface : palette.on_surface_variant;
        if (drop) {
            color = palette.on_primary_container;
        }
        if (crumb.label.empty()) {
            icons.draw(renderer, crumb.icon, x + kInset, top + ((kBoxHeight - kIconSize) / 2.0F),
                       kIconSize, last ? palette.primary : color);
        } else {
            renderer.draw_text(crumb.label, x + kInset, bounds.y + 12.0F, kTextSize, color,
                               width - (2.0F * kInset), ui::Ellipsis::middle);
        }
        shown_.push_back({.crumb = i, .rect = box});
        x += width;
    }

    renderer.fill_rect(
        {bounds.x, bounds.y + kTitleHeight - 1.0F, bounds.width, 1.0F / renderer.scale()},
        palette.outline_variant);
}

std::optional<std::filesystem::path> Breadcrumb::path_at(float x, float y) const {
    const std::optional<std::size_t> crumb = crumb_at(x, y);
    return crumb ? std::optional{crumbs_[*crumb].path} : std::nullopt;
}

void Breadcrumb::set_drop_target(std::optional<std::filesystem::path> path) {
    if (path != drop_target_) {
        drop_target_ = std::move(path);
        redraw_();
    }
}

std::optional<std::size_t> Breadcrumb::crumb_at(float x, float y) const {
    for (const Shown& shown : shown_) {
        if (shown.crumb && x >= shown.rect.x && x < shown.rect.right() && y >= shown.rect.y &&
            y < shown.rect.bottom()) {
            return shown.crumb;
        }
    }
    return std::nullopt;
}

bool Breadcrumb::clickable_at(float x, float y) const {
    const std::optional<std::size_t> crumb = crumb_at(x, y);
    return crumb && *crumb + 1 < crumbs_.size();
}

bool Breadcrumb::claims(float x, float y) const {
    return !bounds_.empty() && x >= bounds_.x && x < bounds_.right() && y >= bounds_.y &&
           y < bounds_.bottom();
}

void Breadcrumb::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    std::optional<std::size_t> over;
    if (event.kind != Kind::leave) {
        over = crumb_at(event.x, event.y);
    }
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left || !over ||
        *over + 1 >= crumbs_.size()) {
        return;  // the last step is the folder shown
    }
    const std::filesystem::path path = crumbs_[*over].path;
    const std::string child = crumbs_[*over + 1].path.filename().string();
    if (go_) {
        go_(path, child);
    }
}

std::vector<std::string> Breadcrumb::shown() const {
    std::vector<std::string> out;
    out.reserve(shown_.size());
    for (const Shown& shown : shown_) {
        if (!shown.crumb) {
            out.emplace_back(kEllipsis);
        } else if (crumbs_[*shown.crumb].label.empty()) {
            out.emplace_back(crumbs_[*shown.crumb].icon);
        } else {
            out.push_back(crumbs_[*shown.crumb].label);
        }
    }
    return out;
}

}  // namespace rummage::views
