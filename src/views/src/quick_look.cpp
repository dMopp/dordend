#include "nordend/views/quick_look.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <unordered_set>
#include <variant>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw_helpers.hpp"
#include "nordend/core/log.hpp"
#include "nordend/fs/icon.hpp"
#include "nordend/views/format.hpp"
#include "nordend/core/i18n.hpp"

namespace nordend::views {
namespace {

// Milliseconds from then to now.
double since(QuickLook::Clock::time_point then) {
    return std::chrono::duration<double, std::milli>(QuickLook::Clock::now() - then).count();
}

constexpr float kMargin = 32.0F;         // around the image
constexpr float kCaptionHeight = 44.0F;  // under it
constexpr float kRadius = 6.0F;
constexpr float kCardWidth = 340.0F;
constexpr float kCardIcon = 96.0F;
constexpr float kRowHeight = 24.0F;
constexpr auto kFade = std::chrono::milliseconds{140};

// The text page.
constexpr float kTextSize = 13.0F;
constexpr float kLineHeight = QuickLook::kTextLineHeight;
constexpr float kPagePadding = 16.0F;
constexpr float kGutterGap = 16.0F;     // between the line numbers and the text
constexpr float kPageColumns = 120.0F;  // characters a page is wide, at most
// Selecting past the lines' top or bottom scrolls: this many logical pixels a second for each
// pixel beyond, at most kAutoscrollMax.
constexpr float kAutoscrollPerPixel = 12.0F;
constexpr float kAutoscrollMax = 4000.0F;
constexpr float kPageRadius = 10.0F;
constexpr float kScrollbarWidth = 4.0F;
constexpr float kScrollbarMinThumb = 24.0F;

// Where a shadow falls: a little below what casts it.
ui::Rect shifted_down(ui::Rect rect) {
    rect.y += 6.0F;
    return rect;
}

float fade_at(QuickLook::Clock::time_point opened, QuickLook::Clock::time_point now) {
    const auto elapsed = std::chrono::duration<float>(now - opened).count();
    return std::clamp(elapsed / std::chrono::duration<float>(kFade).count(), 0.0F, 1.0F);
}

// A caption in a pill under area.
void draw_caption(ui::Renderer& renderer, const ui::Rect& area, const theme::Palette& palette,
                  const std::string& caption, float fade) {
    const float text_width_px = std::min(text_width(renderer, caption, 13.0F), area.width - 32.0F);
    const ui::Rect pill{area.x + ((area.width - text_width_px) / 2.0F) - 14.0F,
                        area.bottom() + 12.0F, text_width_px + 28.0F, 26.0F};
    renderer.draw_box(pill, {.fill = palette.surface_container_high.with_alpha(0.92F * fade),
                             .radius = ui::Corners::all(13.0F)});
    renderer.draw_text(caption, pill.x + 14.0F, pill.y + 5.0F, 13.0F,
                       palette.on_surface.with_alpha(fade), text_width_px, ui::Ellipsis::middle);
}

}  // namespace

QuickLook::QuickLook(core::WorkerPool& pool, std::shared_ptr<const fs::MimeDatabase> mime,
                     std::shared_ptr<const thumbs::Cache> thumbnails, std::function<void()> redraw)
    : pool_{pool},
      mime_{std::move(mime)},
      thumbnails_{std::move(thumbnails)},
      redraw_{std::move(redraw)} {}

QuickLook::~QuickLook() {
    *alive_ = false;
    for (auto& [key, slot] : slots_) {
        drop(slot);
    }
}

const preview::Provider& QuickLook::provider_of(const fs::Entry& entry) const {
    return preview::provider_for(*mime_, folder_->mime_of(entry));
}

// The provider is part of the key: an entry whose type becomes known (sniffed) gets a new slot.
std::string QuickLook::key_of(const fs::Entry& entry) const {
    return std::format("{}\n{}\n{}", folder_->path_of(entry).string(), entry.modified_ns,
                       static_cast<int>(provider_of(entry).kind()));
}

void QuickLook::drop(Slot& slot) {
    for (std::optional<core::CancelToken>* job : {&slot.thumbnail_job, &slot.content_job}) {
        if (*job) {
            (*job)->cancel();
            job->reset();
        }
    }
    for (std::unique_ptr<ui::Texture>* texture : {&slot.thumbnail_texture, &slot.image_texture}) {
        if (*texture) {
            retired_.push_back(std::move(*texture));
        }
    }
}

void QuickLook::open(std::shared_ptr<Folder> folder, Clock::time_point now) {
    opened_ = now;
    follow(std::move(folder));
}

void QuickLook::close() {
    for (auto& [key, slot] : slots_) {
        drop(slot);
    }
    slots_.clear();
    folder_.reset();
    shown_name_.clear();
    shown_key_.clear();
    selection_.clear();
    selecting_ = false;
    layout_ = {};
    bottom_for_.clear();
    redraw_();
}

QuickLook::Slot& QuickLook::ensure(const fs::Entry& entry, bool current) {
    const preview::Provider& provider = provider_of(entry);
    const std::string key = key_of(entry);
    Slot& slot = slots_[key];
    slot.path = folder_->path_of(entry);
    slot.kind = provider.kind();
    if (!slot.content && !slot.content_job) {
        slot.requested = Clock::now();
        slot.in_advance = !current;
        slot.content_job = pool_.submit(
            current ? core::Priority::visible : core::Priority::nearby,
            [&provider,
             request = preview::Request{.path = slot.path,
                                        .mime = std::string{mime_->name(folder_->mime_of(entry))},
                                        .width = target_width_,
                                        .height = target_height_}](const core::CancelToken& token) {
                return std::make_shared<preview::Content>(provider.load(request, token));
            },
            [this, alive = alive_, key](std::shared_ptr<preview::Content> content) {
                if (!*alive) {
                    return;
                }
                const auto it = slots_.find(key);
                if (it == slots_.end()) {
                    return;
                }
                it->second.content_job.reset();
                it->second.content = std::move(content);
                redraw_();
            });
    }
    // The first stage of an image, while the sharp one is made: the thumbnail, usually cached
    // already.
    if (current && slot.kind == preview::Kind::image && thumbnails_ && !slot.content &&
        !slot.thumbnail_job && !slot.thumbnail_pixels && !slot.thumbnail_texture) {
        slot.thumbnail_job = pool_.submit(
            core::Priority::visible,
            [cache = thumbnails_, path = slot.path](const core::CancelToken& /*token*/) {
                return std::make_shared<picture::Image>(
                    cache->get(path, thumbs::Size::large).image);
            },
            [this, alive = alive_, key](std::shared_ptr<picture::Image> pixels) {
                if (!*alive) {
                    return;
                }
                const auto it = slots_.find(key);
                if (it == slots_.end()) {
                    return;
                }
                it->second.thumbnail_job.reset();
                if (!pixels->empty()) {
                    it->second.thumbnail_pixels = std::move(pixels);
                    redraw_();
                }
            });
    }
    return slot;
}

void QuickLook::follow(std::shared_ptr<Folder> folder) {
    if (!folder) {
        close();
        return;
    }
    folder_ = std::move(folder);
    const fs::Entry* const entry = folder_->cursor_entry();
    const std::optional<std::size_t> cursor = folder_->cursor();
    if (entry == nullptr || !cursor) {
        close();
        return;
    }
    // Checked again on every call (each frame while open): an entry's type may only be known
    // once its content has been sniffed, and the key changes then.
    const std::string key = key_of(*entry);
    const bool moved = key != shown_key_;
    shown_name_ = entry->name;
    shown_key_ = key;
    const std::vector<fs::Entry>& entries = folder_->entries();
    const std::size_t index = *cursor;
    if (moved) {
        folder_->sniff(index == 0 ? 0 : index - 1, index + 2);
        text_scroller_.jump_to(0.0F);
        text_sideways_.jump_to(0.0F);
        selection_.clear();
        selecting_ = false;
        layout_ = {};
    }
    std::unordered_set<std::string> keep{key};
    ensure(*entry, true);
    // The neighbours, so that the next arrow finds them ready.
    for (const std::size_t neighbour : {index - 1, index + 1}) {
        if (neighbour < entries.size()) {
            ensure(entries[neighbour], false);
            keep.insert(key_of(entries[neighbour]));
        }
    }
    for (auto it = slots_.begin(); it != slots_.end();) {
        if (keep.contains(it->first)) {
            ++it;
        } else {
            drop(it->second);
            it = slots_.erase(it);
        }
    }
    if (moved) {
        redraw_();
    }
}

const preview::Text* QuickLook::shown_text() const {
    const preview::Content* const shown = content();
    return shown == nullptr ? nullptr : std::get_if<preview::Text>(shown);
}

bool QuickLook::pointer(const ui::PointerEvent& event, Clock::time_point now) {
    const preview::Text* const text = shown_text();
    if (text == nullptr) {
        return false;
    }
    using Kind = ui::PointerEvent::Kind;
    switch (event.kind) {
        case Kind::scroll:
            // Shift turns the wheel sideways; a touchpad and a tilting wheel go both ways.
            if (event.modifiers.shift && event.wheel_y != 0.0F) {
                text_sideways_.wheel(event.wheel_y, now);
            } else {
                if (event.wheel_y != 0.0F) {
                    text_scroller_.wheel(event.wheel_y, now);
                } else if (event.scroll_y != 0.0F) {
                    text_scroller_.finger(event.scroll_y, now);
                }
                if (event.wheel_x != 0.0F) {
                    text_sideways_.wheel(event.wheel_x, now);
                } else if (event.scroll_x != 0.0F) {
                    text_sideways_.finger(event.scroll_x, now);
                }
            }
            if (event.scroll_finger && event.scroll_stop) {
                text_scroller_.release(now);
                text_sideways_.release(now);
            }
            if (selecting_) {
                selection_.drag(point_at(pointer_x_, pointer_y_, *text), text->lines);
            }
            redraw_();
            return false;
        case Kind::press:
            if (event.button != ui::PointerButton::left || !layout_.valid ||
                !layout_.page.contains(event.x, event.y)) {
                return false;
            }
            selection_.press(point_at(event.x, event.y, *text), event.click_count,
                             event.modifiers.shift, text->lines);
            selecting_ = true;
            pointer_x_ = event.x;
            pointer_y_ = event.y;
            autoscrolled_ = now;
            redraw_();
            return true;
        case Kind::motion:
            if (selecting_) {
                pointer_x_ = event.x;
                pointer_y_ = event.y;
                selection_.drag(point_at(event.x, event.y, *text), text->lines);
                autoscrolled_ = now;
                redraw_();
            }
            return false;
        case Kind::release:
            if (event.button == ui::PointerButton::left) {
                selecting_ = false;
            }
            return false;
        case Kind::enter:
        case Kind::leave:
            return false;
    }
    return false;
}

TextPoint QuickLook::point_at(float x, float y, const preview::Text& text) const {
    if (text.lines.empty() || layout_.char_width <= 0.0F) {
        return {};
    }
    // Above the lines: the first line's start; below: the last line's end.
    const float from_top = y - layout_.lines.y + text_scroller_.offset();
    if (from_top < 0.0F) {
        return {};
    }
    const auto line = static_cast<std::size_t>(from_top / kLineHeight);
    if (line >= text.lines.size()) {
        return {text.lines.size() - 1, text.lines.back().size()};
    }
    // The nearer edge of the character under the pointer: half way through one is after it.
    const float columns =
        std::max(0.0F, (x - layout_.text_x + text_sideways_.offset()) / layout_.char_width);
    const auto column = static_cast<std::size_t>(std::lround(columns));
    return {line, byte_at_column(text.lines[line], column)};
}

bool QuickLook::autoscrolling() const {
    if (!selecting_ || !layout_.valid) {
        return false;
    }
    const bool above_below = pointer_y_ < layout_.lines.y || pointer_y_ > layout_.lines.bottom();
    const bool beside =
        text_sideways_.max_offset() > 0.0F &&
        (pointer_x_ < layout_.text_x || pointer_x_ > layout_.text_x + layout_.text_room);
    return above_below || beside;
}

bool QuickLook::text_key(const ui::KeyEvent& event, Clock::time_point now) {
    if (shown_text() == nullptr || !layout_.valid || event.modifiers != ui::Modifiers{}) {
        return false;
    }
    // A page: what is shown less a line, so that one stays in sight.
    const float page = std::max(kLineHeight, layout_.lines.height - kLineHeight);
    const float from = text_scroller_.destination();
    switch (event.keysym) {
        case XKB_KEY_Home:
            text_scroller_.glide_to(0.0F, now);
            break;
        case XKB_KEY_End:
            text_scroller_.glide_to(text_scroller_.max_offset(), now);
            break;
        case XKB_KEY_Page_Up:
            text_scroller_.glide_to(from - page, now);
            break;
        case XKB_KEY_Page_Down:
            text_scroller_.glide_to(from + page, now);
            break;
        default:
            return false;
    }
    redraw_();
    return true;
}

void QuickLook::select_all_text() {
    if (const preview::Text* const text = shown_text()) {
        selection_.select_all(text->lines);
        redraw_();
    }
}

std::string QuickLook::selected_text() const {
    const preview::Text* const text = shown_text();
    return text == nullptr ? std::string{} : selection_.text(text->lines);
}

bool QuickLook::animating(Clock::time_point now) const {
    return is_open() && (fade_at(opened_, now) < 1.0F || text_scroller_.animating() ||
                         text_sideways_.animating() || autoscrolling());
}

bool QuickLook::loading() const {
    const auto it = slots_.find(shown_key_);
    return it != slots_.end() && it->second.content_job.has_value();
}

const preview::Content* QuickLook::content() const {
    const auto it = slots_.find(shown_key_);
    return it == slots_.end() ? nullptr : it->second.content.get();
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> QuickLook::image_size() const {
    const preview::Content* const shown = content();
    const auto* const loaded = shown == nullptr ? nullptr : std::get_if<picture::Loaded>(shown);
    if (loaded == nullptr || loaded->image.empty()) {
        return std::nullopt;
    }
    return std::pair{loaded->image.width, loaded->image.height};
}

void QuickLook::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                     ui::IconSet& icons, Clock::time_point now) {
    retired_.clear();
    if (!is_open()) {
        return;
    }
    const fs::Entry* const entry = folder_->cursor_entry();
    const auto it = slots_.find(shown_key_);
    if (entry == nullptr || it == slots_.end()) {
        return;
    }
    Slot& slot = it->second;
    const float fade = fade_at(opened_, now);
    renderer.fill_rect(bounds, palette.scrim.with_alpha(0.78F * fade));
    const ui::Rect area{bounds.x + kMargin, bounds.y + kMargin, bounds.width - (2.0F * kMargin),
                        bounds.height - (2.0F * kMargin) - kCaptionHeight};
    // Images are made for this area, in physical pixels (from the next one on).
    target_width_ = static_cast<std::uint32_t>(std::max(1.0F, area.width * renderer.scale()));
    target_height_ = static_cast<std::uint32_t>(std::max(1.0F, area.height * renderer.scale()));
    if (slot.kind == preview::Kind::image) {
        draw_image(renderer, area, palette, icons, *entry, slot, fade);
        return;
    }
    const preview::Content* const shown = slot.content.get();
    if (const auto* text = shown == nullptr ? nullptr : std::get_if<preview::Text>(shown)) {
        draw_text(renderer, area, palette, *entry, *text, fade, now);
        return;
    }
    if (slot.kind == preview::Kind::text && shown == nullptr) {
        return;  // a moment: the card would flash before the text
    }
    const auto* const info = shown == nullptr ? nullptr : std::get_if<preview::Info>(shown);
    draw_card(renderer, area, palette, icons, *entry, info, fade);
}

void QuickLook::draw_image(ui::Renderer& renderer, const ui::Rect& area,
                           const theme::Palette& palette, ui::IconSet& icons,
                           const fs::Entry& entry, Slot& slot, float fade) {
    auto* const loaded = slot.content ? std::get_if<picture::Loaded>(slot.content.get()) : nullptr;
    // Upload what arrived: the sharp image replaces the thumbnail. The pixels are on the GPU now:
    // they go (the size stays, for the caption).
    if (loaded != nullptr && !loaded->image.empty() && !slot.image_texture) {
        slot.image_texture = std::make_unique<ui::Texture>(
            loaded->image.width, loaded->image.height, loaded->image.rgba);
        std::vector<std::uint8_t>{}.swap(loaded->image.rgba);
        log::debug("quick look: {} {}x{} on screen {:.1f} ms after it was asked for{}", entry.name,
                   loaded->image.width, loaded->image.height, since(slot.requested),
                   slot.in_advance ? " (made in advance, as a neighbour)" : "");
        if (slot.thumbnail_texture) {
            retired_.push_back(std::move(slot.thumbnail_texture));
        }
        slot.thumbnail_pixels.reset();
    }
    if (slot.thumbnail_pixels && !slot.image_texture) {
        slot.thumbnail_texture = std::make_unique<ui::Texture>(slot.thumbnail_pixels->width,
                                                               slot.thumbnail_pixels->height,
                                                               slot.thumbnail_pixels->rgba);
        slot.thumbnail_pixels.reset();
        if (!slot.thumbnail_logged) {
            slot.thumbnail_logged = true;
            log::debug("quick look: {} thumbnail on screen {:.1f} ms after the image was asked for",
                       entry.name, since(slot.requested));
        }
    }
    const ui::Texture* const texture =
        slot.image_texture ? slot.image_texture.get() : slot.thumbnail_texture.get();

    std::string caption = entry.name;
    if (loaded != nullptr && loaded->full_width > 0) {
        caption += std::format("   {} × {}", loaded->full_width, loaded->full_height);
    }
    caption += "   " + format_size(entry.size);
    if (texture == nullptr) {
        // Nothing yet (or it failed): the file's icon, and why when there is a reason.
        fs::Entry typed = entry;
        typed.mime = folder_->mime_of(entry);
        const float x = area.x + ((area.width - kCardIcon) / 2.0F);
        const float y = area.y + ((area.height - kCardIcon) / 2.0F);
        icons.draw_file(renderer, (*mime_).name(typed.mime), fs::icon_name(*mime_, typed), x, y,
                        kCardIcon, palette.on_surface_variant.with_alpha(fade), typed.folder_icon);
        if (loaded != nullptr && !loaded->error().empty()) {
            draw_centered(renderer, loaded->error(), area, y + kCardIcon + 16.0F, 13.0F,
                          palette.error.with_alpha(fade));
        }
    } else {
        // The sharp image fitted to the area, never larger than its own pixels; a thumbnail
        // fills the area, where the sharp image is likely to land.
        const auto texture_width = static_cast<float>(texture->width());
        const auto texture_height = static_cast<float>(texture->height());
        float fit = std::min(area.width / texture_width, area.height / texture_height);
        if (texture == slot.image_texture.get()) {
            fit = std::min(fit, 1.0F / renderer.scale());
        }
        const float shown_width = texture_width * fit;
        const float shown_height = texture_height * fit;
        const ui::Rect rect{area.x + ((area.width - shown_width) / 2.0F),
                            area.y + ((area.height - shown_height) / 2.0F), shown_width,
                            shown_height};
        renderer.draw_shadow(shifted_down(rect), ui::Corners::all(kRadius), 18.0F,
                             palette.shadow.with_alpha(0.6F * fade));
        renderer.draw_image(*texture, rect, ui::Corners::all(kRadius), fade);
    }
    draw_caption(renderer, area, palette, caption, fade);
}

void QuickLook::draw_text(ui::Renderer& renderer, const ui::Rect& area,
                          const theme::Palette& palette, const fs::Entry& entry,
                          const preview::Text& text, float fade, Clock::time_point now) {
    constexpr auto mono = ui::Typeface::monospace;
    const float scale = renderer.scale();
    // Monospace: every character (and digit) is this wide.
    const float char_width =
        renderer.fonts().layout("0", kTextSize, scale, 0.0F, ui::Ellipsis::end, mono).width / scale;
    const std::size_t count = text.lines.size();
    const std::string last_number = std::to_string(std::max<std::size_t>(count, 1));
    // A log's end has no line numbers: they would count from where the reading started, not
    // from the file's first line (to count those, the whole file would be read).
    const bool numbered = !text.tail;
    const float gutter = numbered ? static_cast<float>(last_number.size()) * char_width : 0.0F;
    const float gutter_gap = numbered ? kGutterGap : 0.0F;
    const float content = static_cast<float>(count) * kLineHeight;
    const float page_width = std::min(
        area.width, (2.0F * kPagePadding) + gutter + gutter_gap + (kPageColumns * char_width));
    const float page_height =
        std::min(area.height, std::max(content, kLineHeight) + (2.0F * kPagePadding));
    const ui::Rect page{area.x + ((area.width - page_width) / 2.0F),
                        area.y + ((area.height - page_height) / 2.0F), page_width, page_height};
    renderer.draw_shadow(shifted_down(page), ui::Corners::all(kPageRadius), 18.0F,
                         palette.shadow.with_alpha(0.6F * fade));
    renderer.draw_box(page, {.fill = palette.surface_container.with_alpha(fade),
                             .radius = ui::Corners::all(kPageRadius)});

    const ui::Rect inner{page.x, page.y + kPagePadding, page.width,
                         page.height - (2.0F * kPagePadding)};
    text_scroller_.set_extent(content, inner.height);
    // A log opens on its newest lines, at the bottom.
    if (text.tail && bottom_for_ != shown_key_) {
        text_scroller_.jump_to(content);
        bottom_for_ = shown_key_;
    }
    text_scroller_.advance(now);
    const float numbers_right = page.x + kPagePadding + gutter;
    const float text_x = numbers_right + gutter_gap;
    const float text_room = page.right() - kPagePadding - text_x;
    // Long lines scroll sideways, to the widest one's end and a character more.
    if (widest_for_ != &text) {
        widest_for_ = &text;
        widest_ = 0;
        for (const std::string& line : text.lines) {
            widest_ = std::max(widest_, column_at_byte(line, line.size()));
        }
    }
    text_sideways_.set_extent(static_cast<float>(widest_ + 1) * char_width, text_room);
    text_sideways_.advance(now);
    layout_ = {.page = page,
               .lines = inner,
               .text_x = text_x,
               .text_room = text_room,
               .char_width = char_width,
               .valid = true};
    // Selecting with the pointer beyond the lines (above, below, beside): the page scrolls that
    // way, faster the farther it is, and the selection follows.
    if (autoscrolling()) {
        const float seconds =
            std::min(std::chrono::duration<float>(now - autoscrolled_).count(), 0.1F);
        const auto step = [&](float beyond) {
            return std::clamp(beyond * kAutoscrollPerPixel, -kAutoscrollMax, kAutoscrollMax) *
                   seconds;
        };
        if (pointer_y_ < inner.y || pointer_y_ > inner.bottom()) {
            const float beyond =
                pointer_y_ < inner.y ? pointer_y_ - inner.y : pointer_y_ - inner.bottom();
            text_scroller_.jump_to(text_scroller_.offset() + step(beyond));
        }
        if (pointer_x_ < text_x || pointer_x_ > text_x + text_room) {
            const float beyond =
                pointer_x_ < text_x ? pointer_x_ - text_x : pointer_x_ - (text_x + text_room);
            text_sideways_.jump_to(text_sideways_.offset() + step(beyond));
        }
        autoscrolled_ = now;
        selection_.drag(point_at(pointer_x_, pointer_y_, text), text.lines);
    }
    const float offset = text_scroller_.offset();
    const float sideways = text_sideways_.offset();
    const ui::Rect text_area{text_x, inner.y, text_room, inner.height};
    // The line box is about 1.2 times the font size: centered in the line height.
    const float baseline_shift = (kLineHeight - (kTextSize * 1.2F)) / 2.0F;
    const theme::SyntaxColors colours = theme::syntax_colors(palette);
    const auto first = static_cast<std::size_t>(std::max(0.0F, offset / kLineHeight));
    const auto last = std::min(
        count, static_cast<std::size_t>(std::ceil((offset + inner.height) / kLineHeight)) + 1);
    // The line numbers stay put; the text beside them scrolls both ways.
    renderer.push_clip(inner);
    for (std::size_t i = first; numbered && i < last; ++i) {
        const float y = inner.y + (static_cast<float>(i) * kLineHeight) - offset + baseline_shift;
        const std::string number = std::to_string(i + 1);
        renderer.draw_text(number, numbers_right - (static_cast<float>(number.size()) * char_width),
                           y, kTextSize, palette.on_surface_variant.with_alpha(0.7F * fade), 0.0F,
                           ui::Ellipsis::end, mono);
    }
    renderer.pop_clip();
    renderer.push_clip(text_area);
    // The selection behind the text: each line's selected characters, and a character's room
    // for its line break when that is selected too.
    if (!selection_.empty()) {
        const auto [from, to] = selection_.range();
        for (std::size_t i = std::max(first, from.line); i < last && i <= to.line; ++i) {
            const std::string_view line = text.lines[i];
            const std::size_t start = i == from.line ? column_at_byte(line, from.byte) : 0;
            const std::size_t end = i == to.line ? column_at_byte(line, to.byte)
                                                 : column_at_byte(line, line.size()) + 1;
            if (end <= start) {
                continue;
            }
            const float x = text_x - sideways + (static_cast<float>(start) * char_width);
            const float width = static_cast<float>(end - start) * char_width;
            renderer.fill_rect(
                {x, inner.y + (static_cast<float>(i) * kLineHeight) - offset, width, kLineHeight},
                palette.secondary_container.with_alpha(fade));
        }
    }
    for (std::size_t i = first; i < last; ++i) {
        if (text.lines[i].empty()) {
            continue;
        }
        const float y = inner.y + (static_cast<float>(i) * kLineHeight) - offset + baseline_shift;
        const std::vector<ui::ColorRun> runs = i < text.spans.size()
                                                   ? colour_runs(text.spans[i], colours, fade)
                                                   : std::vector<ui::ColorRun>{};
        // Whole: the clip cuts it at the page's edge, and it scrolls into sight.
        renderer.draw_text_runs(text.lines[i], text_x - sideways, y, kTextSize,
                                palette.on_surface.with_alpha(fade), runs, 0.0F, ui::Ellipsis::end,
                                mono);
    }
    renderer.pop_clip();
    if (text_sideways_.max_offset() > 0.0F) {
        const float content_width = static_cast<float>(widest_ + 1) * char_width;
        const float thumb = std::max(kScrollbarMinThumb, text_room * text_room / content_width);
        const float travel = text_room - thumb;
        const float thumb_x = text_x + (travel * sideways / text_sideways_.max_offset());
        renderer.draw_box({thumb_x, page.bottom() - kScrollbarWidth - 4.0F, thumb, kScrollbarWidth},
                          {.fill = palette.on_surface_variant.with_alpha(0.45F * fade),
                           .radius = ui::Corners::all(kScrollbarWidth / 2.0F)});
    }
    // Scrollbar: a thin thumb, only when there is something to scroll.
    if (text_scroller_.max_offset() > 0.0F) {
        const float thumb = std::max(kScrollbarMinThumb, inner.height * inner.height / content);
        const float travel = inner.height - thumb;
        const float thumb_y = inner.y + (travel * offset / text_scroller_.max_offset());
        renderer.draw_box({page.right() - kScrollbarWidth - 4.0F, thumb_y, kScrollbarWidth, thumb},
                          {.fill = palette.on_surface_variant.with_alpha(0.45F * fade),
                           .radius = ui::Corners::all(kScrollbarWidth / 2.0F)});
    }

    // name   C++   UTF-8   CRLF   3449 lines   120 kB
    std::string caption = entry.name;
    if (!text.language.empty()) {
        caption += "   " + text.language;
        if (text::looks_minified(text)) {
            caption += i18n::tr(" (minified)");
        }
    }
    caption += "   ";
    caption += text::encoding_name(text.encoding);
    if (text.crlf) {
        caption += "   CRLF";
    }
    caption += "   ";
    if (text.tail) {
        caption += i18n::trf("last {} lines", count);
    } else if (text.truncated) {
        caption += i18n::trf("first {} lines", count);
    } else {
        caption += count == 1 ? std::string{i18n::tr("1 line")} : i18n::trf("{} lines", count);
    }
    caption += "   " + format_size(entry.size);
    draw_caption(renderer, area, palette, caption, fade);
}

void QuickLook::draw_card(ui::Renderer& renderer, const ui::Rect& area,
                          const theme::Palette& palette, ui::IconSet& icons, const fs::Entry& entry,
                          const preview::Info* info, float fade) {
    fs::Entry typed = entry;
    typed.mime = folder_->mime_of(entry);
    const std::string_view type =
        typed.mime == fs::kUnknownMime ? std::string_view{"…"} : mime_->name(typed.mime);

    // The rows: what the directory entry says at once, the rest when read.
    struct Row {
        std::string_view label;
        std::string value;
        ui::Typeface typeface{ui::Typeface::proportional};
    };

    std::vector<Row> rows;
    if (!entry.is_directory()) {
        rows.push_back({.label = "Size", .value = format_size(entry.size)});
    } else if (info != nullptr && info->items) {
        rows.push_back({.label = "Items", .value = std::format("{}", *info->items)});
    }
    rows.push_back({.label = "Modified", .value = format_time(entry.modified_ns)});
    if (info != nullptr && !info->permissions.empty()) {
        rows.push_back({.label = "Permissions",
                        .value = info->permissions,
                        .typeface = ui::Typeface::monospace});
        rows.push_back({.label = "Owner", .value = info->owner + " · " + info->group});
    }
    if (info != nullptr && !info->link_target.empty()) {
        rows.push_back({.label = "Link to", .value = info->link_target});
    }
    const bool error = info != nullptr && !info->error.empty();
    const float height = 32.0F + kCardIcon + 20.0F + 24.0F + 20.0F + 4.0F +
                         (static_cast<float>(rows.size() + (error ? 1 : 0)) * kRowHeight) + 20.0F;
    const ui::Rect card{area.x + ((area.width - kCardWidth) / 2.0F),
                        area.y + ((area.height - height) / 2.0F), kCardWidth, height};
    renderer.draw_shadow(shifted_down(card), ui::Corners::all(14.0F), 18.0F,
                         palette.shadow.with_alpha(0.6F * fade));
    renderer.draw_box(card, {.fill = palette.surface_container_high.with_alpha(fade),
                             .radius = ui::Corners::all(14.0F)});
    float y = card.y + 32.0F;
    icons.draw(
        renderer, fs::icon_name(*mime_, typed), card.x + ((kCardWidth - kCardIcon) / 2.0F), y,
        kCardIcon,
        (entry.is_directory() ? palette.primary : palette.on_surface_variant).with_alpha(fade));
    y += kCardIcon + 20.0F;
    draw_centered(renderer, entry.name, card, y, 15.0F, palette.on_surface.with_alpha(fade));
    y += 24.0F;
    draw_centered(renderer, type, card, y, 12.0F, palette.on_surface_variant.with_alpha(fade));
    y += 20.0F + 4.0F;
    for (const Row& row : rows) {
        const float label_width = renderer.draw_text(i18n::tr(row.label), card.x + 20.0F, y, 13.0F,
                                                     palette.on_surface_variant.with_alpha(fade));
        // Long values (a link target) are cut in the middle, keeping their end.
        const float room = kCardWidth - 40.0F - label_width - 16.0F;
        const float full =
            renderer.fonts()
                .layout(row.value, 13.0F, renderer.scale(), 0.0F, ui::Ellipsis::end, row.typeface)
                .width /
            renderer.scale();
        const float width = std::min(full, room);
        renderer.draw_text(row.value, card.right() - 20.0F - width, y, 13.0F,
                           palette.on_surface.with_alpha(fade), room, ui::Ellipsis::middle,
                           row.typeface);
        y += kRowHeight;
    }
    if (error) {
        draw_centered(renderer, info->error, card, y, 13.0F, palette.error.with_alpha(fade));
    }
}

QuickLook::Usage QuickLook::usage() const {
    Usage usage{.slots = slots_.size(), .texture_bytes = 0, .pixel_bytes = 0};
    for (const auto& [key, slot] : slots_) {
        for (const auto* texture : {slot.thumbnail_texture.get(), slot.image_texture.get()}) {
            usage.texture_bytes += texture != nullptr ? texture->bytes() : 0;
        }
        usage.pixel_bytes += slot.thumbnail_pixels ? slot.thumbnail_pixels->rgba.size() : 0;
        if (slot.content) {
            if (const auto* loaded = std::get_if<picture::Loaded>(slot.content.get())) {
                usage.pixel_bytes += loaded->image.rgba.size();
            }
        }
    }
    return usage;
}

}  // namespace nordend::views
