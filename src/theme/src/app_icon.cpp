#include "rummage/theme/app_icon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <numbers>
#include <string>
#include <string_view>
#include <system_error>

#include "app_icon_template.hpp"
#include "oklch.hpp"
#include "rummage/core/file.hpp"
#include "rummage/core/log.hpp"

namespace rummage::theme {
namespace {

// The icon can carry this much chroma at most (GNOME's orange is about 0.18): above it the
// folder loses its tonal separation.
constexpr double kMaxChroma = 0.16;

// What a recoloured icon's comment says, and how rummage recognises its own file.
constexpr std::string_view kWrittenMark = "written by rummage";

// Hue difference b - a, in (-pi, pi].
double turn(double a, double b) {
    double d = b - a;
    while (d > std::numbers::pi) {
        d -= 2.0 * std::numbers::pi;
    }
    while (d <= -std::numbers::pi) {
        d += 2.0 * std::numbers::pi;
    }
    return d;
}

std::filesystem::path data_home() {
    const char* xdg = std::getenv("XDG_DATA_HOME");  // NOLINT(concurrency-mt-unsafe)
    // The XDG base directory spec ignores relative paths.
    if (xdg != nullptr && xdg[0] == '/') {
        return xdg;
    }
    const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".local" / "share";
    }
    return {};
}

// The icon under an installation prefix (CMakeLists.txt installs it there).
constexpr std::string_view kIconInPrefix = "share/icons/hicolor/scalable/apps/rummage.svg";

}  // namespace

std::string hex(Color color) {
    const auto byte = [](float v) {
        return static_cast<unsigned>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
    };
    return std::format("#{:02x}{:02x}{:02x}", byte(color.r), byte(color.g), byte(color.b));
}

AppIconTones app_icon_tones(Color accent) {
    const Lch base_front = to_lch(kBaseTones.front);
    const Lch base_tab = to_lch(kBaseTones.tab);
    const Lch base_highlight = to_lch(kBaseTones.highlight);
    const Lch lch = to_lch(accent);
    const double chroma = std::min(lch.c, kMaxChroma);
    // Each tone: the installed icon's lightness, its chroma and hue relative to the front.
    const auto tone = [&](const Lch& base) {
        return from_lch(
            {base.l, chroma * (base.c / base_front.c), lch.h + turn(base_front.h, base.h)});
    };
    return {.front = tone(base_front), .tab = tone(base_tab), .highlight = tone(base_highlight)};
}

std::string app_icon_svg(std::optional<Color> accent) {
    if (!accent) {
        return std::string{kAppIconSvg};
    }
    const std::string comment = std::format(
        "<!-- rummage's application icon in the desktop's accent ({}), {} to follow it.\n"
        "     Set icon.follow_accent = false in ~/.config/rummage/config.toml to take it away. "
        "-->\n",
        hex(*accent), kWrittenMark);
    std::string result{kAppIconSvg};
    const auto tones = app_icon_tones(*accent);
    for (const auto& [original, color] :
         {std::pair{"#F7C65B", tones.front}, std::pair{"#CF954B", tones.tab},
          std::pair{"#FFE4A2", tones.highlight}}) {
        const auto replacement = hex(color);
        for (std::size_t at = 0; (at = result.find(original, at)) != std::string::npos;
             at += replacement.size())
            result.replace(at, 7, replacement);
    }
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + comment + result;
}

bool written_by_rummage(const std::string& svg) {
    return svg.find(kWrittenMark) != std::string::npos;
}

AppIconPlace default_app_icon_place() {
    const std::filesystem::path data = data_home();
    if (data.empty()) {
        return {};
    }
    AppIconPlace place{.path = data / "icons" / "hicolor" / "scalable" / "apps" / "rummage.svg"};
    std::error_code error;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        const std::filesystem::path installed = exe.parent_path().parent_path() / kIconInPrefix;
        place.installed_there = std::filesystem::weakly_canonical(installed, error) ==
                                std::filesystem::weakly_canonical(place.path, error);
        place.installed_there = place.installed_there && !error;
    }
    return place;
}

AppIconChange sync_app_icon(const AppIconPlace& place, std::optional<Color> accent) {
    if (place.path.empty()) {
        return AppIconChange::none;
    }
    const core::TextFile current = core::read_text_file(place.path);
    if (!current.error.empty()) {
        log::warn("icon: {}: {}", place.path.string(), current.error);
        return AppIconChange::failed;
    }
    const bool ours = current.exists && written_by_rummage(current.text);
    std::string wanted;
    AppIconChange change = AppIconChange::written;
    if (accent) {
        if (current.exists && !ours && !place.installed_there) {
            return AppIconChange::left_alone;  // someone put their own icon there
        }
        wanted = app_icon_svg(accent);
    } else if (!ours) {
        return AppIconChange::none;  // nothing of ours to take away
    } else if (place.installed_there) {
        wanted = app_icon_svg(std::nullopt);
        change = AppIconChange::restored;
    } else {
        std::error_code error;
        std::filesystem::remove(place.path, error);
        if (error) {
            log::warn("icon: cannot remove {}: {}", place.path.string(), error.message());
            return AppIconChange::failed;
        }
        log::info("icon: removed {}", place.path.string());
        return AppIconChange::removed;
    }
    if (current.exists && current.text == wanted) {
        return AppIconChange::none;
    }
    if (const std::error_code error = core::write_file_atomically(place.path, wanted)) {
        log::warn("icon: cannot write {}: {}", place.path.string(), error.message());
        return AppIconChange::failed;
    }
    log::info("icon: {} {}", change == AppIconChange::restored ? "put back" : "wrote",
              place.path.string());
    return change;
}

AppIconKeeper::AppIconKeeper(core::WorkerPool& pool, AppIconPlace place)
    : pool_{pool}, place_{std::move(place)} {}

AppIconKeeper::~AppIconKeeper() { token_.cancel(); }

void AppIconKeeper::follow(std::optional<Color> accent) {
    if (busy_) {
        pending_ = accent;
        return;
    }
    if (last_ == accent) {
        return;
    }
    start(accent);
}

void AppIconKeeper::start(std::optional<Color> accent) {
    busy_ = true;
    last_ = accent;
    token_ = pool_.submit(
        core::Priority::prefetch,
        [place = place_, accent](const core::CancelToken&) { return sync_app_icon(place, accent); },
        [this](AppIconChange change) {
            busy_ = false;
            if (done_handler_) {
                done_handler_(change);
            }
            if (pending_) {
                const std::optional<Color> next = *pending_;
                pending_.reset();
                follow(next);
            }
        });
}

}  // namespace rummage::theme
