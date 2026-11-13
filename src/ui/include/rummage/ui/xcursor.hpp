#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <wayland-client.h>

#include "rummage/ui/seat.hpp"
#include "rummage/ui/wl_ptr.hpp"

struct wl_cursor_theme;

namespace rummage::ui {

// The cursor names to look for in an xcursor theme, best first: the CSS name of the cursor spec,
// then the X11 names older themes use.
[[nodiscard]] std::span<const std::string_view> xcursor_names(CursorShape shape);

// The cursor size in logical pixels: XCURSOR_SIZE's value when it is a sane number, else 24.
[[nodiscard]] int xcursor_size(const char* value);

// The pointer's image when the compositor does not offer cursor-shape-v1 (GNOME, weston): the
// xcursor theme of XCURSOR_THEME (libwayland-cursor's "default" without it) at XCURSOR_SIZE,
// loaded at the window's scale rounded up, on a surface of rummage's own. A viewport (when the
// compositor offers viewporter) gives the image its logical size whatever size the theme has;
// without one, an image whose size is not a multiple of the scale is shown at scale 1 (a buffer
// scale must divide the buffer). Animated cursors show their first frame.
//
// Not copyable or movable. Must not outlive the Display.
class XcursorFallback {
public:
    // viewporter is optional.
    XcursorFallback(wl_compositor* compositor, wl_shm* shm, wp_viewporter* viewporter);
    ~XcursorFallback();

    XcursorFallback(const XcursorFallback&) = delete;
    XcursorFallback& operator=(const XcursorFallback&) = delete;
    XcursorFallback(XcursorFallback&&) = delete;
    XcursorFallback& operator=(XcursorFallback&&) = delete;

    // Shows shape on pointer; serial is the pointer's last enter. scale: the window's (1.25...).
    void show(wl_pointer* pointer, std::uint32_t serial, CursorShape shape, double scale);

private:
    struct ThemeDeleter {
        void operator()(wl_cursor_theme* theme) const noexcept;
    };

    wl_compositor* compositor_;
    wl_shm* shm_;
    std::string theme_name_;  // empty: libwayland-cursor's default
    int size_;
    int theme_scale_{0};  // the scale theme_ was loaded at; 0 = not loaded
    std::unique_ptr<wl_cursor_theme, ThemeDeleter> theme_;
    // Reverse order = teardown order: the viewport before its surface.
    WlPtr<wl_surface> surface_;
    WlPtr<wp_viewport> viewport_;
};

}  // namespace rummage::ui
