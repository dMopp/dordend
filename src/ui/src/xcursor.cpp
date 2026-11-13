#include "rummage/ui/xcursor.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <wayland-cursor.h>

#include "rummage/core/log.hpp"

namespace rummage::ui {
namespace {

constexpr int kDefaultSize = 24;
constexpr int kMaxSize = 256;
constexpr int kMaxScale = 8;

using namespace std::string_view_literals;

// NOLINTBEGIN(cert-err58-cpp): string_view arrays do not throw
constexpr std::array kStandard{"default"sv, "left_ptr"sv};
constexpr std::array kPointer{"pointer"sv, "hand2"sv, "hand1"sv};
constexpr std::array kText{"text"sv, "xterm"sv};
constexpr std::array kWait{"wait"sv, "watch"sv};
constexpr std::array kProgress{"progress"sv, "left_ptr_watch"sv};
constexpr std::array kGrab{"grab"sv, "openhand"sv, "hand1"sv};
constexpr std::array kGrabbing{"grabbing"sv, "closedhand"sv, "fleur"sv};
constexpr std::array kNotAllowed{"not-allowed"sv, "crossed_circle"sv};
constexpr std::array kEwResize{"ew-resize"sv, "col-resize"sv, "sb_h_double_arrow"sv};
constexpr std::array kNsResize{"ns-resize"sv, "row-resize"sv, "sb_v_double_arrow"sv};
constexpr std::array kNResize{"n-resize"sv, "top_side"sv};
constexpr std::array kSResize{"s-resize"sv, "bottom_side"sv};
constexpr std::array kEResize{"e-resize"sv, "right_side"sv};
constexpr std::array kWResize{"w-resize"sv, "left_side"sv};
constexpr std::array kNeResize{"ne-resize"sv, "top_right_corner"sv};
constexpr std::array kNwResize{"nw-resize"sv, "top_left_corner"sv};
constexpr std::array kSeResize{"se-resize"sv, "bottom_right_corner"sv};
constexpr std::array kSwResize{"sw-resize"sv, "bottom_left_corner"sv};
// NOLINTEND(cert-err58-cpp)

}  // namespace

std::span<const std::string_view> xcursor_names(CursorShape shape) {
    switch (shape) {
        case CursorShape::standard:
            return kStandard;
        case CursorShape::pointer:
            return kPointer;
        case CursorShape::text:
            return kText;
        case CursorShape::wait:
            return kWait;
        case CursorShape::progress:
            return kProgress;
        case CursorShape::grab:
            return kGrab;
        case CursorShape::grabbing:
            return kGrabbing;
        case CursorShape::not_allowed:
            return kNotAllowed;
        case CursorShape::ew_resize:
            return kEwResize;
        case CursorShape::ns_resize:
            return kNsResize;
        case CursorShape::n_resize:
            return kNResize;
        case CursorShape::s_resize:
            return kSResize;
        case CursorShape::e_resize:
            return kEResize;
        case CursorShape::w_resize:
            return kWResize;
        case CursorShape::ne_resize:
            return kNeResize;
        case CursorShape::nw_resize:
            return kNwResize;
        case CursorShape::se_resize:
            return kSeResize;
        case CursorShape::sw_resize:
            return kSwResize;
    }
    return kStandard;
}

int xcursor_size(const char* value) {
    if (value == nullptr) {
        return kDefaultSize;
    }
    const std::string_view text{value};
    int size = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), size);
    if (error != std::errc{} || end != text.data() + text.size() || size <= 0 || size > kMaxSize) {
        return kDefaultSize;
    }
    return size;
}

void XcursorFallback::ThemeDeleter::operator()(wl_cursor_theme* theme) const noexcept {
    wl_cursor_theme_destroy(theme);
}

// NOLINTBEGIN(concurrency-mt-unsafe): the environment is read on the UI thread at startup
XcursorFallback::XcursorFallback(wl_compositor* compositor, wl_shm* shm, wp_viewporter* viewporter)
    : compositor_{compositor},
      shm_{shm},
      theme_name_{std::getenv("XCURSOR_THEME") != nullptr ? std::getenv("XCURSOR_THEME") : ""},
      size_{xcursor_size(std::getenv("XCURSOR_SIZE"))} {
    // NOLINTEND(concurrency-mt-unsafe)
    surface_.reset(wl_compositor_create_surface(compositor_));
    if (viewporter != nullptr) {
        viewport_.reset(wp_viewporter_get_viewport(viewporter, surface_.get()));
    }
}

XcursorFallback::~XcursorFallback() = default;

void XcursorFallback::show(wl_pointer* pointer, std::uint32_t serial, CursorShape shape,
                           double scale) {
    const int buffer_scale = std::clamp(static_cast<int>(std::ceil(scale)), 1, kMaxScale);
    if (buffer_scale != theme_scale_) {
        theme_.reset(wl_cursor_theme_load(theme_name_.empty() ? nullptr : theme_name_.c_str(),
                                          size_ * buffer_scale, shm_));
        theme_scale_ = buffer_scale;
        if (theme_) {
            log::info("cursor: xcursor theme '{}', {} px at scale {}",
                      theme_name_.empty() ? "default" : theme_name_, size_, buffer_scale);
        } else {
            log::warn("cursor: cannot load the xcursor theme '{}'", theme_name_);
        }
    }
    if (!theme_ || !surface_) {
        return;
    }
    wl_cursor* cursor = nullptr;
    for (const std::string_view name : xcursor_names(shape)) {
        const std::string zero_terminated{name};
        cursor = wl_cursor_theme_get_cursor(theme_.get(), zero_terminated.c_str());
        if (cursor != nullptr) {
            break;
        }
    }
    if (cursor == nullptr || cursor->image_count == 0) {
        log::debug("cursor: no '{}' in the theme", xcursor_names(shape).front());
        return;
    }
    wl_cursor_image* image = cursor->images[0];  // NOLINT(*-pointer-arithmetic)
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (buffer == nullptr) {
        return;
    }
    const auto width = static_cast<std::int32_t>(image->width);
    const auto height = static_cast<std::int32_t>(image->height);
    // Image pixels per logical pixel.
    int divisor = buffer_scale;
    if (viewport_) {
        wp_viewport_set_destination(viewport_.get(), std::max(1, width / buffer_scale),
                                    std::max(1, height / buffer_scale));
    } else {
        if (width % buffer_scale != 0 || height % buffer_scale != 0) {
            divisor = 1;  // too large rather than a protocol error
        }
        wl_surface_set_buffer_scale(surface_.get(), divisor);
    }
    wl_surface_attach(surface_.get(), buffer, 0, 0);
    wl_surface_damage_buffer(surface_.get(), 0, 0, width, height);
    wl_surface_commit(surface_.get());
    wl_pointer_set_cursor(pointer, serial, surface_.get(),
                          static_cast<std::int32_t>(image->hotspot_x) / divisor,
                          static_cast<std::int32_t>(image->hotspot_y) / divisor);
}

}  // namespace rummage::ui
