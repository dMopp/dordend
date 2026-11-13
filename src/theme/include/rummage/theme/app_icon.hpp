#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "rummage/core/worker_pool.hpp"
#include "rummage/ui/color.hpp"

// rummage's application icon in the desktop's accent (ROADMAP 9.2). The installed icon
// (data/rummage.svg) stays as it is; rummage writes a recoloured copy where the desktop
// looks first, $XDG_DATA_HOME/icons/hicolor/scalable/apps/rummage.svg, and takes it away when
// told to (icon.follow_accent = false) or when there is no accent to follow.
namespace rummage::theme {

using ui::Color;

// The folder's three tones; the papers and outline keep their original colours.
struct AppIconTones {
    Color front;
    Color tab;
    Color highlight;

    bool operator==(const AppIconTones&) const = default;
};

// The installed icon's tones.
inline constexpr AppIconTones kBaseTones{
    .front = Color::rgb(0xF7C65B), .tab = Color::rgb(0xCF954B), .highlight = Color::rgb(0xFFE4A2)};

// The tones for an accent, in OKLCH: the accent's hue and chroma (chroma held to what the icon
// can carry), each tone keeping the installed icon's lightness and its small turn of hue and
// chroma from the front. The installed front colour as accent gives back kBaseTones.
[[nodiscard]] AppIconTones app_icon_tones(Color accent);

// "#rrggbb".
[[nodiscard]] std::string hex(Color color);

// The icon's SVG. Without an accent: the installed icon, byte for byte data/rummage.svg. With
// one: the recoloured icon, its comment saying rummage wrote it (how rummage knows its own file).
[[nodiscard]] std::string app_icon_svg(std::optional<Color> accent);

// Whether an SVG is one rummage wrote for an accent.
[[nodiscard]] bool written_by_rummage(const std::string& svg);

struct AppIconPlace {
    std::filesystem::path path;  // where the desktop looks first
    // rummage itself is installed under that prefix (cmake --install --prefix ~/.local): the file
    // is also the installed icon, so taking the recoloured one away puts the installed one back.
    bool installed_there{false};
};

// $XDG_DATA_HOME (else ~/.local/share)/icons/hicolor/scalable/apps/rummage.svg, and whether the
// running binary's prefix (/proc/self/exe/../..) holds its icon at that very path. Empty path
// when neither variable is set.
[[nodiscard]] AppIconPlace default_app_icon_place();

enum class AppIconChange : std::uint8_t {
    none,        // already right
    written,     // the recoloured icon written
    removed,     // rummage's recoloured icon taken away
    restored,    // the installed icon put back in place of the recoloured one
    left_alone,  // someone else's icon is there: never touched
    failed,      // could not read or write; the reason is logged
};

// Brings the file in line, writing only what changes. With an accent the recoloured icon goes
// there unless another icon not rummage's is there; without one rummage's recoloured icon is
// taken away (or the installed one put back). Blocking: call it from a worker.
[[nodiscard]] AppIconChange sync_app_icon(const AppIconPlace& place, std::optional<Color> accent);

// Keeps the file in line with the accent from the loop thread: the work runs on the pool, one
// job at a time, the latest accent wins. Must not outlive the pool.
class AppIconKeeper {
public:
    AppIconKeeper(core::WorkerPool& pool, AppIconPlace place);
    ~AppIconKeeper();

    AppIconKeeper(const AppIconKeeper&) = delete;
    AppIconKeeper& operator=(const AppIconKeeper&) = delete;
    AppIconKeeper(AppIconKeeper&&) = delete;
    AppIconKeeper& operator=(AppIconKeeper&&) = delete;

    // The accent to follow; nullopt: the installed icon.
    void follow(std::optional<Color> accent);

    // Called on the loop thread after each sync, with what it did (for tests).
    void set_done_handler(std::function<void(AppIconChange)> handler) {
        done_handler_ = std::move(handler);
    }

private:
    void start(std::optional<Color> accent);

    core::WorkerPool& pool_;
    AppIconPlace place_;
    bool busy_{false};
    std::optional<std::optional<Color>> last_;     // the accent of the last job started
    std::optional<std::optional<Color>> pending_;  // asked while busy
    core::CancelToken token_;
    std::function<void(AppIconChange)> done_handler_;
};

}  // namespace rummage::theme
