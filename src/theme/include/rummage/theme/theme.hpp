#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/file_watcher.hpp"
#include "rummage/theme/desktop.hpp"
#include "rummage/theme/palette.hpp"

namespace rummage::theme {

// The current palette: loaded from colors.toml, reloaded when the file changes (Noctalia rewrites
// it on every theme change). A missing file means the default palette; a file that does not parse
// keeps the palette in use, so a half-written or hand-broken file never flashes the default
// colors. Problems are logged. Without the file, the desktop's look (set_desktop) when known,
// otherwise the default palette.
class Theme {
public:
    using ChangeHandler = std::function<void()>;

    explicit Theme(std::filesystem::path path);

    // Starts following the file. The theme must not outlive the loop.
    void watch(core::EventLoop& loop);

    // What the desktop says about its look; nullopt when it says nothing (no portal). Used while
    // there is no colors.toml.
    void set_desktop(std::optional<Desktop> desktop);

    // Called after the palette changed through a reload or the desktop.
    void set_change_handler(ChangeHandler handler) { change_handler_ = std::move(handler); }

    [[nodiscard]] const Palette& palette() const noexcept { return palette_; }

    // The accent the palette follows: colors.toml's primary, else the desktop's own accent;
    // nullopt with the default palette or a desktop that gives none (ROADMAP 9.2).
    [[nodiscard]] std::optional<Color> accent() const;

    // Reads the file again; public so the reload rules can be tested without inotify.
    void reload();

private:
    // The palette when colors.toml is missing.
    [[nodiscard]] Palette fallback() const;
    void changed();

    std::filesystem::path path_;
    bool from_file_{false};
    std::optional<Desktop> desktop_;
    Palette palette_;
    ChangeHandler change_handler_;
    std::unique_ptr<core::FileWatcher> watcher_;
};

}  // namespace rummage::theme
