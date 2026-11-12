#include "nordend/views/status_bar.hpp"

#include <algorithm>
#include <format>
#include <string_view>
#include <sys/statvfs.h>
#include <utility>

#include "nordend/views/format.hpp"
#include "nordend/core/i18n.hpp"

namespace nordend::views {
namespace {

constexpr float kPadding = 16.0F;
constexpr float kTextSize = 12.0F;
constexpr float kButtonWidth = 30.0F;
constexpr float kButtonHeight = 22.0F;
constexpr float kButtonGap = 2.0F;
constexpr float kHelpGap = 10.0F;  // between the views' buttons and "?"
constexpr float kIconSize = 16.0F;
constexpr float kProgressHeight = 2.0F;
constexpr std::array<std::string_view, StatusBar::kViews + 1> kIcons{
    "layout-columns", "layout-grid", "list-details", "help"};

}  // namespace

std::optional<FreeSpace> free_space_of(const std::filesystem::path& path) {
    struct statvfs info{};
    if (statvfs(path.c_str(), &info) != 0) {
        return std::nullopt;
    }
    return FreeSpace{.free = static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize,
                     .total = static_cast<std::uint64_t>(info.f_blocks) * info.f_frsize};
}

std::string status_of(const Folder& folder) {
    const fs::DirectoryModel& model = folder.model();
    const std::size_t count = folder.entries().size();
    if (model.error()) {
        return model.error().message();
    }
    if (folder.selected_count() > 1) {
        return i18n::trf("{} of {} selected", folder.selected_count(), count);
    }
    if (model.loading()) {
        return i18n::trf("{} items, loading…", count);
    }
    std::string status = count == 1 ? std::string{i18n::tr("1 item")} : i18n::trf("{} items", count);
    if (const std::size_t hidden = folder.hidden_count(); hidden > 0) {
        status += i18n::trf(", {} hidden", hidden);
    }
    return status;
}

StatusBar::StatusBar(core::EventLoop& loop, core::WorkerPool& pool, std::function<void()> redraw,
                     std::function<void(std::size_t view)> choose_view, std::function<void()> help)
    : loop_{loop},
      pool_{pool},
      redraw_{std::move(redraw)},
      choose_view_{std::move(choose_view)},
      help_{std::move(help)} {}

StatusBar::~StatusBar() {
    *alive_ = false;
    loop_.remove(message_timer_);
    if (job_) {
        job_->cancel();
    }
}

void StatusBar::update(const Folder& folder) {
    if (job_ || (known_ && path_ == folder.path() && revision_ == folder.model().revision())) {
        return;  // on its way, or known: a draw after the result checks again
    }
    path_ = folder.path();
    revision_ = folder.model().revision();
    known_ = true;
    job_ = pool_.submit(
        core::Priority::visible,
        [path = path_](const core::CancelToken& /*token*/) { return free_space_of(path); },
        [this, alive = alive_](std::optional<FreeSpace> space) {
            if (!*alive) {
                return;
            }
            job_.reset();
            free_ = space;
            redraw_();
        });
}

void StatusBar::show_message(std::string text, bool error) {
    loop_.remove(message_timer_);
    message_ = std::move(text);
    message_error_ = error;
    message_timer_ = loop_.add_timer(kMessageTime, std::chrono::milliseconds{0}, [this] {
        message_timer_ = 0;
        message_.clear();
        redraw_();
    });
    redraw_();
}

void StatusBar::set_transfer(std::optional<TransferStatus> transfer) {
    transfer_ = std::move(transfer);
    if (!transfer_) {
        cancel_button_ = {};
        cancel_hovered_ = false;
    }
    redraw_();
}

void StatusBar::draw(ui::Renderer& renderer, const ui::Rect& bounds, const theme::Palette& palette,
                     ui::IconSet& icons, const Folder& folder, std::size_t view) {
    bounds_ = bounds;
    update(folder);
    renderer.fill_rect({bounds.x, bounds.y, bounds.width, 1.0F / renderer.scale()},
                       palette.outline_variant);

    // The buttons, from the right: "?", then the views.
    const float top = bounds.y + ((kHeight - kButtonHeight) / 2.0F);
    float x = bounds.right() - kPadding + kButtonGap;
    for (std::size_t i = kViews + 1; i-- > 0;) {
        x -= kButtonWidth + kButtonGap;
        if (i == kViews - 1) {
            x -= kHelpGap;
        }
        buttons_.at(i) = {x, top, kButtonWidth, kButtonHeight};
    }
    for (std::size_t i = 0; i <= kViews; ++i) {
        const ui::Rect& button = buttons_.at(i);
        const bool active = i == view;
        if (active || hovered_ == i) {
            renderer.draw_box(button, {.fill = active ? palette.surface_container_highest
                                                      : palette.surface_container_high,
                                       .radius = ui::Corners::all(6.0F)});
        }
        ui::Color color = hovered_ == i ? palette.on_surface : palette.on_surface_variant;
        if (active) {
            color = palette.primary;
        }
        icons.draw(renderer, kIcons.at(i), button.x + ((kButtonWidth - kIconSize) / 2.0F),
                   button.y + ((kButtonHeight - kIconSize) / 2.0F), kIconSize, color);
    }

    // A transfer: its progress along the top edge, a button to stop it before the views'.
    float text_right = buttons_.front().x - kPadding;
    if (transfer_) {
        if (transfer_->fraction >= 0.0F) {
            renderer.fill_rect(
                {bounds.x, bounds.y, bounds.width * std::clamp(transfer_->fraction, 0.0F, 1.0F),
                 kProgressHeight},
                palette.primary);
        }
        cancel_button_ = {buttons_.front().x - kHelpGap - kButtonWidth, top, kButtonWidth,
                          kButtonHeight};
        if (cancel_hovered_) {
            renderer.draw_box(cancel_button_, {.fill = palette.surface_container_high,
                                               .radius = ui::Corners::all(6.0F)});
        }
        icons.draw(renderer, "x", cancel_button_.x + ((kButtonWidth - kIconSize) / 2.0F),
                   cancel_button_.y + ((kButtonHeight - kIconSize) / 2.0F), kIconSize,
                   cancel_hovered_ ? palette.error : palette.on_surface_variant);
        text_right = cancel_button_.x - kPadding;
    }

    // The message, else a transfer, else the status and the free space; cut in the middle when
    // the buttons leave too little room.
    std::string text = message_;
    ui::Color color = message_error_ ? palette.error : palette.on_surface;
    if (message_.empty() && transfer_) {
        text = transfer_->text;
        color = palette.on_surface;
    } else if (message_.empty()) {
        text = status_of(folder);
        if (free_ && !folder.model().error()) {
            text += i18n::trf("  ·  {} free of {}", format_size(free_->free),
                              format_size(free_->total));
        }
        color = folder.model().error() ? palette.error : palette.on_surface_variant;
    }
    const float room = std::max(1.0F, text_right - (bounds.x + kPadding));
    renderer.draw_text(text, bounds.x + kPadding, bounds.y + ((kHeight - kTextSize) / 2.0F) - 2.0F,
                       kTextSize, color, room, ui::Ellipsis::middle);
}

std::optional<std::size_t> StatusBar::button_at(float x, float y) const {
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        const ui::Rect& button = buttons_.at(i);
        if (x >= button.x && x < button.right() && y >= button.y && y < button.bottom()) {
            return i;
        }
    }
    return std::nullopt;
}

bool StatusBar::clickable_at(float x, float y) const {
    return button_at(x, y).has_value() || cancel_button_.contains(x, y);
}

bool StatusBar::claims(float x, float y) const {
    return !bounds_.empty() && x >= bounds_.x && x < bounds_.right() && y >= bounds_.y &&
           y < bounds_.bottom();
}

void StatusBar::pointer(const ui::PointerEvent& event) {
    using Kind = ui::PointerEvent::Kind;
    const bool on_cancel = event.kind != Kind::leave && !cancel_button_.empty() &&
                           event.x >= cancel_button_.x && event.x < cancel_button_.right() &&
                           event.y >= cancel_button_.y && event.y < cancel_button_.bottom();
    if (on_cancel != cancel_hovered_) {
        cancel_hovered_ = on_cancel;
        redraw_();
    }
    if (on_cancel && event.kind == Kind::press && event.button == ui::PointerButton::left) {
        if (cancel_) {
            cancel_();
        }
        return;
    }
    const std::optional<std::size_t> over =
        event.kind == Kind::leave ? std::nullopt : button_at(event.x, event.y);
    if (over != hovered_) {
        hovered_ = over;
        redraw_();
    }
    if (event.kind != Kind::press || event.button != ui::PointerButton::left || !over) {
        return;
    }
    if (*over == kViews) {
        if (help_) {
            help_();
        }
    } else if (choose_view_) {
        choose_view_(*over);
    }
}

}  // namespace nordend::views
