#pragma once

#include "rummage/core/file_picker.hpp"
#include "rummage/views/browser.hpp"

namespace rummage::views {

bool picker_matches(const core::PickerFilter& filter, const fs::Entry& entry,
                    const fs::MimeDatabase& mime);

struct PickerSelection {
    std::vector<std::string> paths;
    std::string error;
    bool overwrite{};
};

// Resolves and validates a choice without creating or modifying any file.
PickerSelection picker_selection(const core::PickerRequest& request,
                                 const std::filesystem::path& folder,
                                 const std::vector<std::string>& selected,
                                 const std::string& typed);

// Picker controls around an ordinary Browser. Browser and callback must outlive this object.
class FilePicker {
public:
    using Done = std::function<void(core::PickerResult)>;
    FilePicker(Browser& browser, core::PickerRequest request,
               std::shared_ptr<const fs::MimeDatabase> mime, std::function<void()> redraw,
               Done done);
    ~FilePicker();
    void accept();
    void cancel();
    bool key(const ui::KeyEvent& event);
    void pointer(const ui::PointerEvent& event);
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons);
    void paste(std::string_view text);
    std::string copy_name(bool cut);
    [[nodiscard]] ui::CursorShape cursor_at(float x, float y) const;

    [[nodiscard]] bool editing_name() const { return focus_ == 1; }

    [[nodiscard]] const std::string& error() const { return error_; }

    void set_name(std::string name) {
        name_.set_text(std::move(name));
        error_.clear();
        redraw_();
    }

    [[nodiscard]] const char* name_label() const {
        return request_.mode == core::PickerRequest::Mode::save ? "Name"
               : request_.directory                             ? "Folder"
                                                                : "File";
    }

    void set_filter(std::uint32_t index);

private:
    void sync_selection();
    void finish(std::vector<std::string> paths);
    void filters_menu(bool keyboard);
    void choices_menu(bool keyboard);
    Browser& browser_;
    core::PickerRequest request_;
    std::shared_ptr<const fs::MimeDatabase> mime_;
    std::function<void()> redraw_;
    Done done_;
    ui::TextField name_;
    ContextMenu menu_;
    ConfirmDialog confirm_;
    std::string error_;
    ui::Rect footer_;
    std::array<ui::Rect, 6> controls_{};
    int focus_{};  // browser, name, filter, choices, cancel, accept
    bool browser_pressed_{};
    std::vector<std::string> last_selection_;
};
}  // namespace rummage::views
