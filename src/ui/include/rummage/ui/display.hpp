#pragma once

#include <cstdint>
#include <wayland-client.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/ui/wl_ptr.hpp"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace rummage::ui {

// Connection to the Wayland compositor and the globals rummage binds.
// Throws std::runtime_error if the connection fails or a required global is missing.
// Not copyable or movable: listeners keep a pointer to this object.
class Display {
public:
    Display();
    ~Display() = default;

    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    Display(Display&&) = delete;
    Display& operator=(Display&&) = delete;

    // Hooks the Wayland socket into the loop (libwayland's prepare_read / read_events protocol).
    // A lost connection quits the loop with exit code 1.
    void attach(core::EventLoop& loop);

    [[nodiscard]] wl_display* native() const noexcept { return display_.get(); }

    [[nodiscard]] int fd() const noexcept { return wl_display_get_fd(display_.get()); }

    [[nodiscard]] wl_compositor* compositor() const noexcept { return compositor_.get(); }

    [[nodiscard]] xdg_wm_base* wm_base() const noexcept { return wm_base_.get(); }

    // Optional: nullptr when the compositor does not offer server-side decorations.
    [[nodiscard]] zxdg_decoration_manager_v1* decoration_manager() const noexcept {
        return decoration_manager_.get();
    }

    // Shared memory buffers: the fallback cursor's images. Every compositor offers it.
    [[nodiscard]] wl_shm* shm() const noexcept { return shm_.get(); }

    // Optional: nullptr on a compositor without input devices.
    [[nodiscard]] wl_seat* seat() const noexcept { return seat_.get(); }

    // Optional: without it the pointer keeps whatever cursor the compositor shows.
    [[nodiscard]] wp_cursor_shape_manager_v1* cursor_shape_manager() const noexcept {
        return cursor_shape_manager_.get();
    }

    // Optional: fractional scaling needs both; without them windows render at scale 1.
    [[nodiscard]] wp_viewporter* viewporter() const noexcept { return viewporter_.get(); }

    [[nodiscard]] wp_fractional_scale_manager_v1* fractional_scale_manager() const noexcept {
        return fractional_scale_manager_.get();
    }

    // Optional: without it, copy and paste stay inside rummage.
    [[nodiscard]] wl_data_device_manager* data_device_manager() const noexcept {
        return data_device_manager_.get();
    }

    // Optional: without it, applications rummage starts may open without the focus.
    [[nodiscard]] xdg_activation_v1* activation() const noexcept { return activation_.get(); }

    [[nodiscard]] zxdg_importer_v2* importer() const noexcept { return importer_.get(); }

    [[nodiscard]] xdg_wm_dialog_v1* dialog_manager() const noexcept {
        return dialog_manager_.get();
    }

private:
    static void on_global(void* data, wl_registry* registry, std::uint32_t name,
                          const char* interface, std::uint32_t version);
    static void on_global_remove(void* data, wl_registry* registry, std::uint32_t name);
    static void on_ping(void* data, xdg_wm_base* wm_base, std::uint32_t serial);

    // Reverse order = teardown order: globals first, the connection last.
    WlPtr<wl_display> display_;
    WlPtr<wl_registry> registry_;
    WlPtr<wl_compositor> compositor_;
    WlPtr<wl_shm> shm_;
    WlPtr<xdg_wm_base> wm_base_;
    WlPtr<zxdg_decoration_manager_v1> decoration_manager_;
    WlPtr<wp_viewporter> viewporter_;
    WlPtr<wp_fractional_scale_manager_v1> fractional_scale_manager_;
    WlPtr<wp_cursor_shape_manager_v1> cursor_shape_manager_;
    WlPtr<xdg_activation_v1> activation_;
    WlPtr<wl_data_device_manager> data_device_manager_;
    WlPtr<wl_seat> seat_;
    WlPtr<zxdg_importer_v2> importer_;
    WlPtr<xdg_wm_dialog_v1> dialog_manager_;
};

}  // namespace rummage::ui
