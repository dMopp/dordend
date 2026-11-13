#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/theme/palette.hpp"
#include "rummage/ui/geometry.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/views/folder.hpp"

namespace rummage::views {

// The space on the file system that holds a folder.
struct FreeSpace {
    std::uint64_t free{0};  // what an unprivileged user may still write
    std::uint64_t total{0};

    bool operator==(const FreeSpace&) const = default;
};

// statvfs(path); nullopt when it fails.
[[nodiscard]] std::optional<FreeSpace> free_space_of(const std::filesystem::path& path);

// A copy or a move in progress, as the bar shows it.
struct TransferStatus {
    std::string text;       // "Copying 163 files · 2.1 of 3.4 GB · 180 MB/s · 8 s left"
    float fraction{-1.0F};  // done, 0 to 1; negative: not known yet
};

// "12 items", "12 items, 3 hidden", "3 of 12 selected", "12 items, loading…", or the folder's
// error.
[[nodiscard]] std::string status_of(const Folder& folder);

// The bar under the view: at the left the folder's status and the free space of its file system
// (read on a worker when the folder or its listing changes, so it follows copies into it), or for
// a few seconds a message ("Opening photo.orf with darktable…"); at the right a button for each
// view, the one shown marked, and "?" for the shortcuts.
//
// Loop thread only. Must not outlive the loop or the pool.
class StatusBar {
public:
    static constexpr float kHeight = 30.0F;
    static constexpr std::size_t kViews = 3;  // columns, grid, details: the browser's modes
    static constexpr std::chrono::seconds kMessageTime{4};

    // choose_view(i): a view's button was clicked; help: the "?" button was.
    StatusBar(core::EventLoop& loop, core::WorkerPool& pool, std::function<void()> redraw,
              std::function<void(std::size_t view)> choose_view, std::function<void()> help);
    ~StatusBar();

    StatusBar(const StatusBar&) = delete;
    StatusBar& operator=(const StatusBar&) = delete;
    StatusBar(StatusBar&&) = delete;
    StatusBar& operator=(StatusBar&&) = delete;

    // Logical pixels (8-32). The bar grows to fit the selected font's line height.
    void set_text_size(float size);

    [[nodiscard]] float text_size() const noexcept { return text_size_; }

    [[nodiscard]] float height(ui::Renderer& renderer) const;

    // view: the index of the view shown.
    void draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
              ui::IconSet& icons, const Folder& folder, std::size_t view);

    // Reads the free space of folder unless it is known for its current listing (draw() does
    // this; tests call it without drawing).
    void update(const Folder& folder);

    [[nodiscard]] bool claims(float x, float y) const;

    // A button is there (a view's, "?", a transfer's ×), as last drawn: the pointer is a hand.
    [[nodiscard]] bool clickable_at(float x, float y) const;

    // Shows text in place of the status for kMessageTime; a new message replaces it. error
    // draws it in the error color.
    void show_message(std::string text, bool error);

    // The message shown; empty when there is none.
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    // A transfer in progress (nullopt: none): its line in place of the status, a thin bar along
    // the top edge, and a button to stop it (cancel). A message still comes first.
    void set_transfer(std::optional<TransferStatus> transfer);

    [[nodiscard]] const std::optional<TransferStatus>& transfer() const noexcept {
        return transfer_;
    }

    void set_cancel_handler(std::function<void()> cancel) { cancel_ = std::move(cancel); }

    void pointer(const ui::PointerEvent& event);

    // The free space as last read; nullopt before it arrives or when it cannot be read.
    [[nodiscard]] const std::optional<FreeSpace>& free_space() const noexcept { return free_; }

    // The free space is being read (snapshots, tests).
    [[nodiscard]] bool loading() const noexcept { return job_.has_value(); }

private:
    // A button: a view's (0 to kViews - 1) or help (kViews).
    [[nodiscard]] std::optional<std::size_t> button_at(float x, float y) const;

    core::EventLoop& loop_;
    core::WorkerPool& pool_;
    std::function<void()> redraw_;
    std::function<void(std::size_t)> choose_view_;
    std::function<void()> help_;

    // What free_ is about: the folder and its listing's revision.
    std::filesystem::path path_;
    std::uint64_t revision_{0};
    bool known_{false};
    std::optional<FreeSpace> free_;
    std::optional<core::CancelToken> job_;

    std::optional<TransferStatus> transfer_;
    std::function<void()> cancel_;
    ui::Rect cancel_button_{};  // as last drawn; empty without a transfer
    bool cancel_hovered_{false};
    std::string message_;
    bool message_error_{false};
    core::EventLoop::SourceId message_timer_{0};

    ui::Rect bounds_{};
    float text_size_{12.0F};
    std::array<ui::Rect, kViews + 1> buttons_{};  // as last drawn
    std::optional<std::size_t> hovered_;
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::views
