#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/text_field.hpp"

namespace rummage::views {

// Single-item properties and a staged folder-icon chooser. All disk work runs on workers.
class PropertiesDialog {
public:
    PropertiesDialog(core::WorkerPool& pool, std::shared_ptr<const fs::MimeDatabase> mime,
                     std::function<void()> redraw);
    ~PropertiesDialog();
    void open(std::filesystem::path path, std::function<void()> changed, bool editable = true);
    void close();

    [[nodiscard]] bool is_open() const { return open_; }

    [[nodiscard]] bool loading() const { return loading_ || (picking_ && icons_loading_); }

    [[nodiscard]] bool saving() const { return saving_; }

    [[nodiscard]] bool picking() const { return picking_; }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    [[nodiscard]] const std::string& icon() const { return draft_; }

    [[nodiscard]] const std::string& error() const { return error_; }

    [[nodiscard]] std::uint64_t generation() const { return generation_; }

    [[nodiscard]] bool can_edit_icon() const;

    [[nodiscard]] bool dirty() const { return draft_ != original_; }

    // Also used by keyboard interaction/tests; choosing only changes the draft.
    void choose_icon(std::string icon);
    void pick_icon();
    void apply();
    bool key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    void paste(std::string_view text);
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons);

private:
    struct Data {
        fs::Entry entry;
        preview::Info info;
        std::string icon;
        std::string icon_error;
        std::string default_icon;
        std::uint64_t device{0}, inode{0};
    };

    struct Control {
        std::string id;
        ui::Rect rect;
        bool enabled{true};
    };

    void activate(std::string_view id);
    void filter();
    void load_catalogue(std::string theme);
    void accept_custom();
    [[nodiscard]] std::vector<std::string> focus_order() const;
    core::WorkerPool& pool_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    std::function<void()> redraw_, changed_;
    core::CancelToken load_job_, save_job_, icons_job_;
    bool open_{false}, loading_{false}, saving_{false}, picking_{false}, editable_{true};
    bool icons_loading_{false};
    std::filesystem::path path_;
    Data data_;
    std::string original_, draft_, error_, focus_{"close"};
    std::optional<std::string> catalogue_theme_;
    std::string resolved_theme_{"hicolor"};
    std::vector<std::string> catalogue_, filtered_;
    std::size_t selected_{0}, page_{0};
    ui::TextField search_, custom_;
    std::vector<Control> controls_;
    ui::Rect grid_{};
    ui::Rect content_{};
    float scroll_{0}, max_scroll_{0};
    std::uint64_t generation_{0};
    std::unique_ptr<ui::IconSet> preview_icons_;
    static constexpr std::size_t kPageSize = 15;
};

}  // namespace rummage::views
