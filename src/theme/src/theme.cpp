#include "rummage/theme/theme.hpp"

#include <string>
#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::theme {
namespace {

void log_problems(const PaletteResult& result) {
    for (const std::string& problem : result.problems) {
        log::warn("theme: {}", problem);
    }
}

}  // namespace

Theme::Theme(std::filesystem::path path) : path_{std::move(path)} {
    const PaletteResult result = load_palette(path_);
    log_problems(result);
    palette_ = result.palette;
    from_file_ = result.from_file;
    log::info("theme: {}", result.from_file ? path_.string() : "default palette");
}

void Theme::set_desktop(std::optional<Desktop> desktop) {
    if (desktop == desktop_) {
        return;
    }
    desktop_ = desktop;
    if (desktop_) {
        log::info("theme: desktop is {}{}", desktop_->dark ? "dark" : "light",
                  desktop_->accent ? ", with an accent colour" : "");
    }
    if (from_file_) {
        return;  // colors.toml wins
    }
    palette_ = fallback();
    changed();
}

std::optional<Color> Theme::accent() const {
    if (from_file_) {
        return palette_.primary.with_alpha(1.0F);
    }
    return desktop_ ? desktop_->accent : std::nullopt;
}

Palette Theme::fallback() const { return desktop_ ? desktop_palette(*desktop_) : Palette{}; }

void Theme::changed() {
    if (change_handler_) {
        change_handler_();
    }
}

void Theme::watch(core::EventLoop& loop) {
    if (path_.empty()) {
        return;
    }
    watcher_ = std::make_unique<core::FileWatcher>(loop, path_, [this] { reload(); });
}

void Theme::reload() {
    const PaletteResult result = load_palette(path_);
    log_problems(result);
    if (result.from_file) {
        log::info("theme: reloaded {}", path_.string());
    } else if (result.problems.empty()) {
        log::info("theme: {} removed, {} palette", path_.string(),
                  desktop_ ? "the desktop's" : "default");
    } else {
        log::warn("theme: keeping the current palette");
        return;
    }
    from_file_ = result.from_file;
    palette_ = from_file_ ? result.palette : fallback();
    changed();
}

}  // namespace rummage::theme
