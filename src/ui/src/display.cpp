#include "rummage/ui/display.hpp"

#include <algorithm>
#include <poll.h>
#include <stdexcept>
#include <string_view>

#include "rummage/core/log.hpp"

namespace rummage::ui {
namespace {

// Highest version of each global whose events we handle. Binding above this would let the
// compositor send events our listeners leave as null pointers.
constexpr std::uint32_t kCompositorVersion = 4;  // wl_surface.damage_buffer
constexpr std::uint32_t kWmBaseVersion = 5;      // xdg_toplevel.wm_capabilities
constexpr std::uint32_t kDecorationManagerVersion = 1;
constexpr std::uint32_t kViewporterVersion = 1;
constexpr std::uint32_t kFractionalScaleManagerVersion = 1;
constexpr std::uint32_t kShmVersion = 1;
constexpr std::uint32_t kSeatVersion = 8;  // wl_pointer.axis_value120 (high-resolution wheels)
constexpr std::uint32_t kCursorShapeManagerVersion = 1;
constexpr std::uint32_t kActivationVersion = 1;
constexpr std::uint32_t kDataDeviceManagerVersion = 3;  // wl_data_device.release

template <class T>
T* bind(wl_registry* registry, std::uint32_t name, const wl_interface& interface,
        std::uint32_t advertised, std::uint32_t supported) {
    const std::uint32_t version = std::min(advertised, supported);
    log::debug("bind {} v{} (compositor offers v{})", interface.name, version, advertised);
    return static_cast<T*>(wl_registry_bind(registry, name, &interface, version));
}

}  // namespace

Display::Display() : display_{wl_display_connect(nullptr)} {
    if (!display_) {
        throw std::runtime_error{"cannot connect to the Wayland display (is WAYLAND_DISPLAY set?)"};
    }

    static constexpr wl_registry_listener kRegistryListener{
        .global = &Display::on_global,
        .global_remove = &Display::on_global_remove,
    };
    registry_.reset(wl_display_get_registry(display_.get()));
    wl_registry_add_listener(registry_.get(), &kRegistryListener, this);
    if (wl_display_roundtrip(display_.get()) < 0) {
        throw std::runtime_error{"Wayland roundtrip failed while reading the registry"};
    }

    if (!compositor_) {
        throw std::runtime_error{"compositor does not offer wl_compositor"};
    }
    if (!wm_base_) {
        throw std::runtime_error{"compositor does not offer xdg_wm_base (xdg-shell)"};
    }
    if (!decoration_manager_) {
        log::info("compositor does not offer xdg-decoration: rummage draws its own title bar");
    }
    if (!viewporter_ || !fractional_scale_manager_) {
        log::warn("compositor does not offer fractional-scale + viewporter: rendering at scale 1");
    }
}

void Display::attach(core::EventLoop& loop) {
    wl_display* wl = display_.get();
    const auto lost = [&loop] {
        log::error("lost the connection to the compositor");
        loop.quit(1);
    };
    loop.add_fd(
        fd(), POLLIN,
        [wl, lost](short revents) {
            if ((revents & POLLIN) != 0) {
                wl_display_read_events(wl);
            } else {
                wl_display_cancel_read(wl);
            }
            if (wl_display_dispatch_pending(wl) < 0 || (revents & (POLLERR | POLLHUP)) != 0) {
                lost();
            }
        },
        {.before_poll =
             [wl, lost] {
                 // Dispatch what is already queued until the read can be prepared, then flush.
                 while (wl_display_prepare_read(wl) != 0) {
                     if (wl_display_dispatch_pending(wl) < 0) {
                         lost();
                         return;
                     }
                 }
                 wl_display_flush(wl);
             },
         .notify_always = true});
}

void Display::on_global(void* data, wl_registry* registry, std::uint32_t name,
                        const char* interface, std::uint32_t version) {
    auto& self = *static_cast<Display*>(data);
    const std::string_view iface{interface};

    if (iface == wl_compositor_interface.name) {
        self.compositor_.reset(bind<wl_compositor>(registry, name, wl_compositor_interface, version,
                                                   kCompositorVersion));
    } else if (iface == xdg_wm_base_interface.name) {
        self.wm_base_.reset(
            bind<xdg_wm_base>(registry, name, xdg_wm_base_interface, version, kWmBaseVersion));
        static constexpr xdg_wm_base_listener kWmBaseListener{.ping = &Display::on_ping};
        xdg_wm_base_add_listener(self.wm_base_.get(), &kWmBaseListener, &self);
    } else if (iface == zxdg_decoration_manager_v1_interface.name) {
        self.decoration_manager_.reset(
            bind<zxdg_decoration_manager_v1>(registry, name, zxdg_decoration_manager_v1_interface,
                                             version, kDecorationManagerVersion));
    } else if (iface == wl_shm_interface.name) {
        self.shm_.reset(bind<wl_shm>(registry, name, wl_shm_interface, version, kShmVersion));
    } else if (iface == wl_seat_interface.name && !self.seat_) {
        // One seat is all a desktop file manager needs.
        self.seat_.reset(bind<wl_seat>(registry, name, wl_seat_interface, version, kSeatVersion));
    } else if (iface == wp_cursor_shape_manager_v1_interface.name) {
        self.cursor_shape_manager_.reset(
            bind<wp_cursor_shape_manager_v1>(registry, name, wp_cursor_shape_manager_v1_interface,
                                             version, kCursorShapeManagerVersion));
    } else if (iface == zxdg_importer_v2_interface.name) {
        self.importer_.reset(
            bind<zxdg_importer_v2>(registry, name, zxdg_importer_v2_interface, version, 1));
    } else if (iface == xdg_wm_dialog_v1_interface.name) {
        self.dialog_manager_.reset(
            bind<xdg_wm_dialog_v1>(registry, name, xdg_wm_dialog_v1_interface, version, 1));
    } else if (iface == xdg_activation_v1_interface.name) {
        self.activation_.reset(bind<xdg_activation_v1>(registry, name, xdg_activation_v1_interface,
                                                       version, kActivationVersion));
    } else if (iface == wl_data_device_manager_interface.name) {
        self.data_device_manager_.reset(bind<wl_data_device_manager>(
            registry, name, wl_data_device_manager_interface, version, kDataDeviceManagerVersion));
    } else if (iface == wp_viewporter_interface.name) {
        self.viewporter_.reset(bind<wp_viewporter>(registry, name, wp_viewporter_interface, version,
                                                   kViewporterVersion));
    } else if (iface == wp_fractional_scale_manager_v1_interface.name) {
        self.fractional_scale_manager_.reset(bind<wp_fractional_scale_manager_v1>(
            registry, name, wp_fractional_scale_manager_v1_interface, version,
            kFractionalScaleManagerVersion));
    }
}

void Display::on_global_remove(void* /*data*/, wl_registry* /*registry*/, std::uint32_t name) {
    // The globals we bind are never hot-unplugged; outputs and seats will be handled when
    // rummage starts tracking them.
    log::debug("global {} removed", name);
}

void Display::on_ping(void* /*data*/, xdg_wm_base* wm_base, std::uint32_t serial) {
    xdg_wm_base_pong(wm_base, serial);
}

}  // namespace rummage::ui
