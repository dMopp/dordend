#include "rummage/ui/wl_ptr.hpp"

namespace rummage::ui {

void destroy(wl_display* display) noexcept { wl_display_disconnect(display); }

void destroy(wl_registry* registry) noexcept { wl_registry_destroy(registry); }

void destroy(wl_compositor* compositor) noexcept { wl_compositor_destroy(compositor); }

void destroy(wl_shm* shm) noexcept { wl_shm_destroy(shm); }

void destroy(wl_surface* surface) noexcept { wl_surface_destroy(surface); }

void destroy(wl_callback* callback) noexcept { wl_callback_destroy(callback); }

void destroy(wl_seat* seat) noexcept {
    if (wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
        wl_seat_release(seat);
    } else {
        wl_seat_destroy(seat);
    }
}

void destroy(wl_keyboard* keyboard) noexcept {
    if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) {
        wl_keyboard_release(keyboard);
    } else {
        wl_keyboard_destroy(keyboard);
    }
}

void destroy(wl_egl_window* window) noexcept { wl_egl_window_destroy(window); }

void destroy(xdg_wm_base* wm_base) noexcept { xdg_wm_base_destroy(wm_base); }

void destroy(xdg_surface* surface) noexcept { xdg_surface_destroy(surface); }

void destroy(xdg_toplevel* toplevel) noexcept { xdg_toplevel_destroy(toplevel); }

void destroy(zxdg_importer_v2* importer) noexcept { zxdg_importer_v2_destroy(importer); }

void destroy(zxdg_imported_v2* imported) noexcept { zxdg_imported_v2_destroy(imported); }

void destroy(xdg_wm_dialog_v1* manager) noexcept { xdg_wm_dialog_v1_destroy(manager); }

void destroy(xdg_dialog_v1* dialog) noexcept { xdg_dialog_v1_destroy(dialog); }

void destroy(zxdg_decoration_manager_v1* manager) noexcept {
    zxdg_decoration_manager_v1_destroy(manager);
}

void destroy(zxdg_toplevel_decoration_v1* decoration) noexcept {
    zxdg_toplevel_decoration_v1_destroy(decoration);
}

void destroy(wp_viewporter* viewporter) noexcept { wp_viewporter_destroy(viewporter); }

void destroy(wp_viewport* viewport) noexcept { wp_viewport_destroy(viewport); }

void destroy(wp_fractional_scale_manager_v1* manager) noexcept {
    wp_fractional_scale_manager_v1_destroy(manager);
}

void destroy(wp_fractional_scale_v1* scale) noexcept { wp_fractional_scale_v1_destroy(scale); }

void destroy(xdg_activation_v1* activation) noexcept { xdg_activation_v1_destroy(activation); }

void destroy(xdg_activation_token_v1* token) noexcept { xdg_activation_token_v1_destroy(token); }

void destroy(wl_data_device_manager* manager) noexcept { wl_data_device_manager_destroy(manager); }

void destroy(wl_data_device* device) noexcept {
    if (wl_data_device_get_version(device) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
        wl_data_device_release(device);
    } else {
        wl_data_device_destroy(device);
    }
}

void destroy(wl_data_source* source) noexcept { wl_data_source_destroy(source); }

void destroy(wl_pointer* pointer) noexcept {
    if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION) {
        wl_pointer_release(pointer);
    } else {
        wl_pointer_destroy(pointer);
    }
}

void destroy(wp_cursor_shape_manager_v1* manager) noexcept {
    wp_cursor_shape_manager_v1_destroy(manager);
}

void destroy(wp_cursor_shape_device_v1* device) noexcept {
    wp_cursor_shape_device_v1_destroy(device);
}

}  // namespace rummage::ui
