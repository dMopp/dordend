#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/picture/image.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/scroller.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/texture.hpp"
#include "rummage/views/folder.hpp"
#include "rummage/views/text_selection.hpp"

namespace rummage::views {

// Quick Look: an overlay showing the entry under a folder's cursor, large. It follows the
// cursor: the view underneath keeps the keys, so arrows walk the folder and Quick Look shows
// where they land. What it shows comes from preview::provider_for: images and camera raw files
// fitted to the window, first from their thumbnail (at once) then sharp; text files on a page
// that scrolls with the wheel or the touchpad; an information card for the rest. The entries
// before and after are prepared in advance.
//
// Loop thread only. Textures are created while drawing.
class QuickLook {
public:
    using Clock = ui::Scroller::Clock;

    static constexpr float kTextLineHeight = 20.0F;  // a text page's line, logical pixels

    // thumbnails may be nullptr (no first, blurry stage).
    QuickLook(core::WorkerPool& pool, std::shared_ptr<const fs::MimeDatabase> mime,
              std::shared_ptr<const thumbs::Cache> thumbnails, std::function<void()> redraw);
    ~QuickLook();

    QuickLook(const QuickLook&) = delete;
    QuickLook& operator=(const QuickLook&) = delete;
    QuickLook(QuickLook&&) = delete;
    QuickLook& operator=(QuickLook&&) = delete;

    // Opens on folder's cursor entry; does nothing when there is none.
    void open(std::shared_ptr<Folder> folder, Clock::time_point now);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return folder_ != nullptr; }

    // Shows what folder's cursor is on now (it may have moved, or the view may show another
    // folder); closes when it is on nothing.
    void follow(std::shared_ptr<Folder> folder);

    // Wheel and touchpad scroll a text page; on it, the left button selects text (drag, double
    // click a word, triple click a line, Shift+click to extend). True when a press was taken
    // (on the page); the owner does what it wants with the others (a click elsewhere closes).
    bool pointer(const ui::PointerEvent& event, Clock::time_point now);

    // On a text page: Home and End go to its top and bottom, Page Up and Page Down a page up
    // and down. False for other keys, or when no text page is shown.
    bool text_key(const ui::KeyEvent& event, Clock::time_point now);

    // A text page is shown (Ctrl+A and Ctrl+C are then its own).
    [[nodiscard]] bool shows_text() const { return shown_text() != nullptr; }

    // Selects the whole text shown.
    void select_all_text();

    // The text selected; empty when nothing is.
    [[nodiscard]] std::string selected_text() const;

    // The selection on the text page (tests).
    [[nodiscard]] const TextSelection& text_selection() const noexcept { return selection_; }

    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, Clock::time_point now);

    // The fade-in or a text page's scrolling is running.
    [[nodiscard]] bool animating(Clock::time_point now) const;

    // The shown entry's content is still being made (snapshots, tests).
    [[nodiscard]] bool loading() const;

    // The shown entry's content once made; nullptr before (tests).
    [[nodiscard]] const preview::Content* content() const;

    // The name of the entry shown; empty when closed.
    [[nodiscard]] const std::string& shown() const noexcept { return shown_name_; }

    // The shown text page's scrolling (the snapshot tool's benchmark), and sideways.
    [[nodiscard]] ui::Scroller& text_scroller() noexcept { return text_scroller_; }

    [[nodiscard]] ui::Scroller& text_sideways() noexcept { return text_sideways_; }

    // Size of the sharp image of the shown entry once made (tests).
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> image_size() const;

    // What it holds (the memory report): slots, GPU texture bytes, pixel bytes kept in memory.
    struct Usage {
        std::size_t slots{0};
        std::size_t texture_bytes{0};
        std::size_t pixel_bytes{0};
    };

    [[nodiscard]] Usage usage() const;

private:
    struct Slot {
        std::filesystem::path path;
        preview::Kind kind{preview::Kind::info};
        std::optional<core::CancelToken> thumbnail_job;
        std::optional<core::CancelToken> content_job;
        std::shared_ptr<picture::Image> thumbnail_pixels;
        std::shared_ptr<preview::Content> content;  // once made (for images, the sharp stage)
        std::unique_ptr<ui::Texture> thumbnail_texture;
        std::unique_ptr<ui::Texture> image_texture;
        // When the content was asked for, and whether as the entry shown or a neighbour made in
        // advance: the debug log says how long it took (ROADMAP 7.6).
        Clock::time_point requested;
        bool in_advance{false};
        bool thumbnail_logged{false};
    };

    [[nodiscard]] const preview::Provider& provider_of(const fs::Entry& entry) const;
    [[nodiscard]] std::string key_of(const fs::Entry& entry) const;
    [[nodiscard]] const preview::Text* shown_text() const;
    Slot& ensure(const fs::Entry& entry, bool current);
    void drop(Slot& slot);
    void draw_image(ui::Renderer& renderer, const ui::Rect& area, const theme::Palette& palette,
                    ui::IconSet& icons, const fs::Entry& entry, Slot& slot, float fade);
    void draw_text(ui::Renderer& renderer, const ui::Rect& area, const theme::Palette& palette,
                   const fs::Entry& entry, const preview::Text& text, float fade,
                   Clock::time_point now);
    void draw_card(ui::Renderer& renderer, const ui::Rect& area, const theme::Palette& palette,
                   ui::IconSet& icons, const fs::Entry& entry, const preview::Info* info,
                   float fade);

    core::WorkerPool& pool_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    std::shared_ptr<const thumbs::Cache> thumbnails_;
    std::function<void()> redraw_;

    std::shared_ptr<Folder> folder_;  // nullptr: closed
    std::string shown_name_;
    std::string shown_key_;
    Clock::time_point opened_;
    std::unordered_map<std::string, Slot> slots_;  // the shown entry and its neighbours
    ui::Scroller text_scroller_;                   // the shown text page
    ui::Scroller text_sideways_;                   // its long lines, sideways
    // The widest line of the text it was counted for, in characters.
    const preview::Text* widest_for_{nullptr};
    std::size_t widest_{0};
    std::string bottom_for_;   // the log last opened at its bottom (its key)
    TextSelection selection_;  // on the shown text page

    // The text page as last drawn, for the pointer: the lines' area, where text starts, a
    // character's width; valid once drawn.
    struct TextLayout {
        ui::Rect page;
        ui::Rect lines;
        float text_x{0.0F};
        float text_room{0.0F};  // the text's width on the page (line numbers apart)
        float char_width{0.0F};
        bool valid{false};
    };

    TextLayout layout_;
    bool selecting_{false};  // the left button is down on the page
    float pointer_y_{0.0F};  // while selecting: above or below the lines scrolls
    float pointer_x_{0.0F};
    Clock::time_point autoscrolled_;  // the last frame that scrolled while selecting
    [[nodiscard]] TextPoint point_at(float x, float y, const preview::Text& text) const;
    [[nodiscard]] bool autoscrolling() const;
    // Textures of slots dropped outside a draw: deleted at the next draw, with GL current.
    std::vector<std::unique_ptr<ui::Texture>> retired_;
    // The size images are made for: the last drawn image area, in physical pixels.
    std::uint32_t target_width_{2560};
    std::uint32_t target_height_{1440};
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::views
