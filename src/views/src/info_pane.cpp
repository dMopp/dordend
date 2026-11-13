#include "rummage/views/info_pane.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include "draw_helpers.hpp"
#include "rummage/fs/icon.hpp"
#include "rummage/views/format.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {

constexpr float kPadding = 20.0F;     // either side of the content
constexpr float kMaxColumn = 360.0F;  // the content is at most this wide, centered
constexpr float kTop = 24.0F;
constexpr float kIconSize = 64.0F;
constexpr float kMaxPreview = 0.5F;  // of the pane's height, for a thumbnail
constexpr float kRowHeight = 22.0F;
constexpr float kTextSize = 13.0F;
constexpr float kSmallSize = 12.0F;
constexpr float kSectionSize = 11.0F;
constexpr float kLineSize = 11.0F;  // a text file's lines
constexpr float kLineHeight = 15.0F;
constexpr float kPagePadding = 8.0F;
constexpr std::size_t kPageLines = 14;  // at most, under the name

// "1 file", "3 files".
std::string count_of(std::size_t count, std::string_view noun) {
    if (noun == "folder") {
        return count == 1 ? std::string{i18n::tr("1 folder")} : i18n::trf("{} folders", count);
    }
    if (noun == "file") {
        return count == 1 ? std::string{i18n::tr("1 file")} : i18n::trf("{} files", count);
    }
    return count == 1 ? std::string{i18n::tr("1 item")} : i18n::trf("{} items", count);
}

// "1/2000 s · F5.6 · ISO 200", leaving out what is missing.
std::string exposure_of(const picture::PhotoInfo& photo) {
    std::string out;
    const auto add = [&](const std::string& part) {
        if (!part.empty()) {
            out += (out.empty() ? "" : " · ") + part;
        }
    };
    add(photo.exposure_time);
    add(photo.aperture);
    add(photo.iso.empty() ? photo.iso : "ISO " + photo.iso);
    return out;
}

std::string rating_of(int rating) {
    if (rating < 0) {
        return std::string{i18n::tr("Rejected")};
    }
    std::string stars;
    for (int i = 0; i < 5; ++i) {
        stars += i < rating ? "★" : "☆";
    }
    return stars;
}

// Lays the pane out top to bottom: one column of centered content.
class Writer {
public:
    Writer(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette)
        : renderer_{renderer},
          palette_{palette},
          width_{std::min(bounds.width - (2.0F * kPadding), kMaxColumn)},
          left_{bounds.x + ((bounds.width - width_) / 2.0F)},
          y_{bounds.y + kTop} {}

    [[nodiscard]] float left() const { return left_; }

    [[nodiscard]] float width() const { return width_; }

    [[nodiscard]] float y() const { return y_; }

    void skip(float height) { y_ += height; }

    void centered(std::string_view text, float size, ui::Color color, float advance) {
        const float w = std::min(text_width(renderer_, text, size), width_);
        renderer_.draw_text(text, left_ + ((width_ - w) / 2.0F), y_, size, color, width_,
                            ui::Ellipsis::middle);
        y_ += advance;
    }

    // A section: a thin line, then its title.
    void section(std::string_view title) {
        y_ += 10.0F;
        renderer_.fill_rect({left_, y_, width_, 1.0F / renderer_.scale()},
                            palette_.outline_variant);
        y_ += 12.0F;
        renderer_.draw_text(i18n::tr(title), left_, y_, kSectionSize, palette_.on_surface_variant);
        y_ += 20.0F;
    }

    // A label at the left, its value at the right (cut in the middle when long); nothing when the
    // value is empty.
    void row(std::string_view label, const std::string& value,
             ui::Typeface typeface = ui::Typeface::proportional) {
        if (value.empty()) {
            return;
        }
        const float label_width =
            renderer_.draw_text(i18n::tr(label), left_, y_, kTextSize, palette_.on_surface_variant);
        const float room = std::max(1.0F, width_ - label_width - 12.0F);
        const float full =
            renderer_.fonts()
                .layout(value, kTextSize, renderer_.scale(), 0.0F, ui::Ellipsis::end, typeface)
                .width /
            renderer_.scale();
        const float shown = std::min(full, room);
        renderer_.draw_text(value, left_ + width_ - shown, y_, kTextSize, palette_.on_surface, room,
                            ui::Ellipsis::middle, typeface);
        y_ += kRowHeight;
    }

private:
    ui::Renderer& renderer_;
    const theme::Palette& palette_;
    float width_;
    float left_;
    float y_;
};

}  // namespace

InfoPane::InfoPane(core::WorkerPool& pool, Thumbnails* thumbnails, std::function<void()> redraw)
    : pool_{pool}, thumbnails_{thumbnails}, redraw_{std::move(redraw)} {}

InfoPane::~InfoPane() {
    *alive_ = false;
    if (job_) {
        job_->cancel();
    }
}

void InfoPane::set_width(float width) {
    width_ = std::clamp(width, kMinWidth, kMaxWidth);
    redraw_();
}

float InfoPane::width_in(float view_width) const {
    const float width = std::min(width_, view_width - kMinContent);
    return width >= kMinWidth ? width : 0.0F;
}

void InfoPane::set_expanded(bool expanded) {
    expanded_ = expanded;
    redraw_();
}

bool InfoPane::scroll(const ui::PointerEvent& event, Clock::time_point now) {
    if (event.x < bounds_.x || event.x >= bounds_.right() || event.y < bounds_.y ||
        event.y >= bounds_.bottom()) {
        return false;
    }
    if (event.wheel_y != 0.0F) {
        scroller_.wheel(event.wheel_y, now);
    } else if (event.scroll_y != 0.0F) {
        scroller_.finger(event.scroll_y, now);
    }
    if (event.scroll_finger && event.scroll_stop) {
        scroller_.release(now);
    }
    redraw_();
    return true;
}

bool InfoPane::click(float x, float y) {
    if (toggle_.width <= 0.0F || x < toggle_.x || x >= toggle_.right() || y < toggle_.y ||
        y >= toggle_.bottom()) {
        return false;
    }
    set_expanded(!expanded_);
    return true;
}

void InfoPane::request(const std::filesystem::path& path, std::int64_t modified_ns,
                       std::string mime, preview::Kind kind) {
    std::string key = std::format("{}\n{}\n{}", path.string(), modified_ns, static_cast<int>(kind));
    if (key == key_) {
        return;
    }
    key_ = std::move(key);
    summary_.reset();
    scroller_.jump_to(0.0F);  // another entry starts at the top
    if (job_) {
        job_->cancel();
    }
    job_ = pool_.submit(
        core::Priority::visible,
        [path, mime = std::move(mime), kind](const core::CancelToken& token) {
            return std::make_shared<const preview::Summary>(
                preview::summarize(path, mime, kind, token));
        },
        [this, alive = alive_, key = key_](std::shared_ptr<const preview::Summary> summary) {
            if (!*alive || key != key_) {
                return;
            }
            job_.reset();
            summary_ = std::move(summary);
            redraw_();
        });
}

void InfoPane::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                    ui::IconSet& icons, const Folder& folder, Clock::time_point now) {
    toggle_ = {};
    bounds_ = bounds;
    // Scrolls when the content (as tall as at the last draw) does not fit.
    scroller_.set_extent(content_height_, bounds.height);
    scroller_.advance(now);
    const float offset = scroller_.offset();
    renderer.push_clip(bounds);
    Writer out{renderer, {bounds.x, bounds.y - offset, bounds.width, bounds.height}, palette};
    const auto finish = [&] {
        content_height_ = out.y() - (bounds.y - offset) + kTop;
        renderer.pop_clip();
    };
    const auto icon = [&](std::string_view name, ui::Color color) {
        icons.draw(renderer, name, out.left() + ((out.width() - kIconSize) / 2.0F), out.y(),
                   kIconSize, color);
        out.skip(kIconSize + 18.0F);
    };

    const fs::Entry* const single = folder.single_selection();
    if (single == nullptr && folder.selected_count() > 1) {
        // Several entries: how many, of which kind, and how big.
        const SelectionSummary& selection = folder.summary();
        icon("files", palette.on_surface_variant);
        out.centered(count_of(selection.folders + selection.files, "item"), 15.0F,
                     palette.on_surface, 22.0F);
        std::string kinds;
        if (selection.folders > 0) {
            kinds = count_of(selection.folders, "folder");
        }
        if (selection.files > 0) {
            kinds += (kinds.empty() ? "" : ", ") + count_of(selection.files, "file");
        }
        out.centered(kinds, kSmallSize, palette.on_surface_variant, 20.0F);
        if (selection.files > 0) {
            out.section("INFORMATION");
            out.row(selection.folders > 0 ? "Size of the files" : "Size",
                    format_size(selection.bytes));
        }
        finish();
        return;
    }

    const fs::MimeDatabase& mime = folder.mime();
    const std::filesystem::path path = single == nullptr ? folder.path() : folder.path_of(*single);
    fs::Entry typed;
    preview::Kind kind = preview::Kind::info;
    if (single != nullptr) {
        typed = *single;
        typed.mime = folder.mime_of(*single);
        kind = preview::provider_for(mime, typed.mime).kind();
    }
    request(path, single == nullptr ? 0 : single->modified_ns,
            single == nullptr ? std::string{} : std::string{mime.name(typed.mime)}, kind);
    const preview::Summary* const summary = summary_.get();
    const preview::Info* const info = summary == nullptr ? nullptr : &summary->info;
    const picture::PhotoInfo* const photo =
        summary == nullptr || !summary->photo ? nullptr : &*summary->photo;

    // The preview takes the pane's whole width (a wider pane, a larger photo); the rows keep to
    // the column.
    const float preview_width = bounds.width - (2.0F * kPadding);
    const float preview_left = bounds.x + kPadding;
    if (single == nullptr) {
        icon("folder", palette.primary);
    } else if (kind == preview::Kind::image) {
        // A square as wide as the pane (at most half its height), always reserved: nothing
        // moves when the thumbnail arrives.
        const float box = std::min(preview_width, bounds.height * kMaxPreview);
        const ui::Texture* thumbnail = nullptr;
        if (thumbnails_ != nullptr) {
            thumbnail = thumbnails_->get(
                path, single->modified_ns,
                thumbs::size_for(static_cast<std::uint32_t>(std::lround(box * renderer.scale()))),
                mime.name(typed.mime));
        }
        if (thumbnail != nullptr) {
            const float fit =
                box / static_cast<float>(std::max(thumbnail->width(), thumbnail->height()));
            const float w = static_cast<float>(thumbnail->width()) * fit;
            const float h = static_cast<float>(thumbnail->height()) * fit;
            const ui::Rect rect{preview_left + ((preview_width - w) / 2.0F),
                                out.y() + ((box - h) / 2.0F), w, h};
            renderer.draw_shadow({rect.x, rect.y + 4.0F, rect.width, rect.height}, {}, 12.0F,
                                 palette.shadow.with_alpha(0.5F));
            renderer.draw_image(*thumbnail, rect, {});
        } else {
            icons.draw_file(renderer, (mime).name(typed.mime), fs::icon_name(mime, typed),
                            out.left() + ((out.width() - kIconSize) / 2.0F),
                            out.y() + ((box - kIconSize) / 2.0F), kIconSize,
                            palette.on_surface_variant, typed.folder_icon);
        }
        out.skip(box + 18.0F);
    } else if (summary != nullptr && summary->text && !summary->text->lines.empty()) {
        // The first lines on a small page.
        const std::vector<std::string>& lines = summary->text->lines;
        const std::vector<std::vector<text::Span>>& spans = summary->text->spans;
        const theme::SyntaxColors colours = theme::syntax_colors(palette);
        const std::size_t count = std::min(lines.size(), kPageLines);
        const float height = (static_cast<float>(count) * kLineHeight) + (2.0F * kPagePadding);
        const ui::Rect page{preview_left, out.y(), preview_width, height};
        renderer.draw_box(page, {.fill = palette.surface_container, .radius = {}});
        for (std::size_t i = 0; i < count; ++i) {
            const std::vector<ui::ColorRun> runs = i < spans.size()
                                                       ? colour_runs(spans[i], colours, 1.0F)
                                                       : std::vector<ui::ColorRun>{};
            renderer.draw_text_runs(lines[i], page.x + kPagePadding,
                                    page.y + kPagePadding + (static_cast<float>(i) * kLineHeight),
                                    kLineSize, palette.on_surface_variant, runs,
                                    page.width - (2.0F * kPagePadding), ui::Ellipsis::end,
                                    ui::Typeface::monospace);
        }
        out.skip(height + 18.0F);
    } else {
        icons.draw_file(renderer, mime.name(typed.mime), fs::icon_name(mime, typed),
                        out.left() + ((out.width() - kIconSize) / 2.0F), out.y(), kIconSize,
                        single->is_directory() ? palette.primary : palette.on_surface_variant,
                        typed.folder_icon);
        out.skip(kIconSize + 18.0F);
    }

    // Name and type.
    std::string name = single == nullptr ? folder.path().filename().string() : single->name;
    if (name.empty()) {
        name = folder.path().string();  // the root
    }
    out.centered(name, 15.0F, palette.on_surface, 22.0F);
    std::string type{i18n::tr("Folder")};
    if (single != nullptr) {
        type =
            typed.mime == fs::kUnknownMime ? std::string{"…"} : std::string{mime.name(typed.mime)};
    }
    out.centered(type, kSmallSize, palette.on_surface_variant, 20.0F);

    // What it is.
    out.section("INFORMATION");
    if (single == nullptr) {
        out.row("Items", std::format("{}", folder.entries().size()));
    } else if (single->is_directory()) {
        // As many as the folder will show when opened.
        out.row("Items",
                info != nullptr && info->items
                    ? std::format("{}", folder.shows_hidden() ? *info->items
                                                              : *info->items - info->hidden_items)
                    : std::string{});
    } else {
        out.row("Size", format_size(single->size));
    }
    if (photo != nullptr && photo->width > 0) {
        out.row("Dimensions", std::format("{} × {}", photo->width, photo->height));
    }
    if (info != nullptr && info->created_ns) {
        out.row("Created", format_time(*info->created_ns));
    }
    if (single != nullptr) {
        out.row("Modified", format_time(single->modified_ns));
        // In the trash: where it was, and when it went.
        if (const fs::TrashedItem* const item = folder.model().trashed(single->name)) {
            out.row("Original location", item->info.original.parent_path().string());
            std::string deleted = item->info.deleted;
            std::ranges::replace(deleted, 'T', ' ');
            out.row("Deleted", deleted);
        }
    }

    // The shot.
    const std::string exposure = photo == nullptr ? std::string{} : exposure_of(*photo);
    if (photo != nullptr && (!photo->camera.empty() || !exposure.empty() || photo->rating)) {
        out.section("PHOTO");
        out.row("Camera", photo->camera);
        out.row("Lens", photo->lens);
        out.row("Exposure", exposure);
        out.row("Focal length", photo->focal_length);
        out.row("Taken", photo->taken);
        out.row("Rating", photo->rating ? rating_of(*photo->rating) : std::string{});
    }

    // The rest, on request.
    if (info != nullptr) {
        out.skip(8.0F);
        const std::string_view label =
            expanded_ ? i18n::tr("Fewer details") : i18n::tr("More details");
        const float label_width = text_width(renderer, label, kTextSize);
        const float total = label_width + 6.0F + 12.0F;
        const float x = out.left() + ((out.width() - total) / 2.0F);
        renderer.draw_text(label, x, out.y(), kTextSize, palette.primary);
        icons.draw(renderer, expanded_ ? "chevron-up" : "chevron-down", x + label_width + 6.0F,
                   out.y() + 2.0F, 12.0F, palette.primary);
        toggle_ = {x - 8.0F, out.y() - 4.0F, total + 16.0F, kRowHeight};
        out.skip(kRowHeight);
        if (expanded_) {
            out.section("MORE");
            out.row("Permissions", info->permissions, ui::Typeface::monospace);
            if (!info->owner.empty()) {
                out.row("Owner", info->owner + " · " + info->group);
            }
            out.row("Location", path.parent_path().string());
            out.row("Link to", info->link_target);
            if (photo != nullptr) {
                out.row("Program", photo->program);
                out.row("Compensation", photo->exposure_bias);
                out.row("Metering", photo->metering);
                out.row("White balance", photo->white_balance);
                out.row("Flash", photo->flash);
                out.row("Software", photo->software);
            }
        }
        if (!info->error.empty()) {
            out.skip(6.0F);
            out.centered(info->error, kSmallSize, palette.error, 20.0F);
        }
    }
    finish();
}

}  // namespace rummage::views
