#include "rummage/ui/seat.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <linux/input-event-codes.h>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <utility>

#include "rummage/core/log.hpp"
#include "rummage/ui/xcursor.hpp"

namespace rummage::ui {
namespace {

// xkbcommon numbers keys as evdev codes + 8.
constexpr xkb_keycode_t kEvdevOffset = 8;
// Repeats delivered per timer wake-up at most, so a stalled loop does not burst-type.
constexpr std::uint64_t kMaxRepeatsPerWake = 2;
// wl_pointer.axis_value120: one wheel notch.
constexpr float kValue120PerNotch = 120.0F;

PointerButton to_button(std::uint32_t code) {
    switch (code) {
        case BTN_LEFT:
            return PointerButton::left;
        case BTN_RIGHT:
            return PointerButton::right;
        case BTN_MIDDLE:
            return PointerButton::middle;
        case BTN_SIDE:
            return PointerButton::back;
        case BTN_EXTRA:
            return PointerButton::forward;
        default:
            return PointerButton::other;
    }
}

std::uint32_t to_wp_shape(CursorShape shape) {
    switch (shape) {
        case CursorShape::standard:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
        case CursorShape::pointer:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
        case CursorShape::text:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
        case CursorShape::wait:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
        case CursorShape::progress:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_PROGRESS;
        case CursorShape::grab:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
        case CursorShape::grabbing:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRABBING;
        case CursorShape::not_allowed:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
        case CursorShape::ew_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE;
        case CursorShape::ns_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE;
        case CursorShape::n_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
        case CursorShape::s_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
        case CursorShape::e_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
        case CursorShape::w_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
        case CursorShape::ne_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
        case CursorShape::nw_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
        case CursorShape::se_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
        case CursorShape::sw_resize:
            return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
    }
    return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
}

// Locale for compose tables: the first of LC_ALL, LC_CTYPE, LANG that is set, else "C".
const char* compose_locale() {
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') {
            return value;
        }
    }
    return "C";
}

// RUMMAGE_FORCE_XCURSOR set and not "0": the xcursor fallback even with cursor-shape-v1.
bool xcursor_forced() {
    const char* value = std::getenv("RUMMAGE_FORCE_XCURSOR");  // NOLINT(concurrency-mt-unsafe)
    return value != nullptr && *value != '\0' && std::string_view{value} != "0";
}

itimerspec milliseconds_to_timer(std::int32_t first, std::int32_t interval) {
    const auto to_timespec = [](std::int32_t ms) {
        return timespec{.tv_sec = ms / 1000, .tv_nsec = static_cast<long>(ms % 1000) * 1'000'000L};
    };
    return {.it_interval = to_timespec(interval), .it_value = to_timespec(first)};
}

}  // namespace

Seat::Seat(Display& display)
    : repeat_fd_{timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)},
      context_{xkb_context_new(XKB_CONTEXT_NO_FLAGS)},
      display_{display} {
    if (repeat_fd_ < 0) {
        throw std::runtime_error{std::string{"timerfd_create: "} + std::strerror(errno)};
    }
    if (!context_) {
        close(repeat_fd_);
        throw std::runtime_error{"xkb_context_new failed"};
    }
    compose_table_.reset(xkb_compose_table_new_from_locale(context_.get(), compose_locale(),
                                                           XKB_COMPOSE_COMPILE_NO_FLAGS));
    if (compose_table_) {
        compose_state_.reset(
            xkb_compose_state_new(compose_table_.get(), XKB_COMPOSE_STATE_NO_FLAGS));
    } else {
        log::warn("no compose table for locale '{}': dead keys disabled", compose_locale());
    }

    if (display.seat() == nullptr) {
        log::warn("compositor offers no wl_seat: no keyboard input");
        return;
    }
    static constexpr wl_seat_listener kSeatListener{
        .capabilities = &Seat::on_capabilities,
        .name = &Seat::on_name,
    };
    wl_seat_add_listener(display.seat(), &kSeatListener, this);
}

Seat::~Seat() {
    xcursor_.reset();
    cursor_device_.reset();
    pointer_.reset();
    keyboard_.reset();
    if (repeat_fd_ >= 0) {
        close(repeat_fd_);
    }
}

void Seat::attach(core::EventLoop& loop) {
    loop.add_fd(repeat_fd_, POLLIN, [this](short /*revents*/) { dispatch_repeat(); });
}

void Seat::on_capabilities(void* data, wl_seat* seat, std::uint32_t capabilities) {
    auto& self = *static_cast<Seat*>(data);
    const bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;
    if (has_keyboard && !self.keyboard_) {
        static constexpr wl_keyboard_listener kKeyboardListener{
            .keymap = &Seat::on_keymap,
            .enter = &Seat::on_keyboard_enter,
            .leave = &Seat::on_keyboard_leave,
            .key = &Seat::on_key,
            .modifiers = &Seat::on_modifiers,
            .repeat_info = &Seat::on_repeat_info,
        };
        self.keyboard_.reset(wl_seat_get_keyboard(seat));
        wl_keyboard_add_listener(self.keyboard_.get(), &kKeyboardListener, &self);
    } else if (!has_keyboard && self.keyboard_) {
        self.disarm_repeat();
        self.keyboard_.reset();
    }

    const bool has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (has_pointer && !self.pointer_) {
        static constexpr wl_pointer_listener kPointerListener{
            .enter = &Seat::on_pointer_enter,
            .leave = &Seat::on_pointer_leave,
            .motion = &Seat::on_pointer_motion,
            .button = &Seat::on_pointer_button,
            .axis = &Seat::on_pointer_axis,
            .frame = &Seat::on_pointer_frame,
            .axis_source = &Seat::on_pointer_axis_source,
            .axis_stop = &Seat::on_pointer_axis_stop,
            .axis_discrete = &Seat::on_pointer_axis_discrete,
            .axis_value120 = &Seat::on_pointer_axis_value120,
            // Not sent below their version (v9, v11); we bind v8.
            .axis_relative_direction = nullptr,
#ifdef WL_POINTER_WARP_SINCE_VERSION
            .warp = nullptr,
#endif
        };
        self.pointer_.reset(wl_seat_get_pointer(seat));
        wl_pointer_add_listener(self.pointer_.get(), &kPointerListener, &self);
        auto* manager = self.display_.cursor_shape_manager();
        if (manager != nullptr && !xcursor_forced()) {
            self.cursor_device_.reset(
                wp_cursor_shape_manager_v1_get_pointer(manager, self.pointer_.get()));
        } else if (self.display_.shm() != nullptr) {
            if (!self.xcursor_) {
                log::info("cursor: {}, drawn from the xcursor theme",
                          manager != nullptr ? "RUMMAGE_FORCE_XCURSOR" : "no cursor-shape-v1");
                self.xcursor_ = std::make_unique<XcursorFallback>(
                    self.display_.compositor(), self.display_.shm(), self.display_.viewporter());
            }
        } else {
            log::warn("compositor offers neither cursor-shape-v1 nor wl_shm: no cursor shapes");
        }
    } else if (!has_pointer && self.pointer_) {
        self.cursor_device_.reset();
        self.pointer_.reset();
        self.pointer_inside_ = false;
    }
}

void Seat::set_cursor(CursorShape shape) {
    if (shape == cursor_) {
        return;
    }
    cursor_ = shape;
    apply_cursor();
}

void Seat::set_cursor_scale(double scale) {
    if (scale == cursor_scale_) {
        return;
    }
    cursor_scale_ = scale;
    if (xcursor_) {
        apply_cursor();
    }
}

void Seat::apply_cursor() {
    if (!pointer_inside_) {
        return;
    }
    if (cursor_device_) {
        wp_cursor_shape_device_v1_set_shape(cursor_device_.get(), pointer_enter_serial_,
                                            to_wp_shape(cursor_));
    } else if (xcursor_ && pointer_) {
        xcursor_->show(pointer_.get(), pointer_enter_serial_, cursor_, cursor_scale_);
    }
}

void Seat::emit_pointer(PointerEvent event) {
    if (state_) {
        event.modifiers = current_modifiers();
    }
    if (pointer_handler_) {
        pointer_handler_(event);
    }
}

void Seat::on_pointer_enter(void* data, wl_pointer* /*pointer*/, std::uint32_t serial,
                            wl_surface* /*surface*/, wl_fixed_t x, wl_fixed_t y) {
    auto& self = *static_cast<Seat*>(data);
    self.pointer_inside_ = true;
    self.pointer_enter_serial_ = serial;
    self.pointer_x_ = static_cast<float>(wl_fixed_to_double(x));
    self.pointer_y_ = static_cast<float>(wl_fixed_to_double(y));
    self.apply_cursor();  // the cursor must be set on every enter
    self.emit_pointer(
        {.kind = PointerEvent::Kind::enter, .x = self.pointer_x_, .y = self.pointer_y_});
}

void Seat::on_pointer_leave(void* data, wl_pointer* /*pointer*/, std::uint32_t /*serial*/,
                            wl_surface* /*surface*/) {
    auto& self = *static_cast<Seat*>(data);
    self.pointer_inside_ = false;
    self.emit_pointer({.kind = PointerEvent::Kind::leave});
}

void Seat::on_pointer_motion(void* data, wl_pointer* /*pointer*/, std::uint32_t /*time*/,
                             wl_fixed_t x, wl_fixed_t y) {
    auto& self = *static_cast<Seat*>(data);
    self.pointer_x_ = static_cast<float>(wl_fixed_to_double(x));
    self.pointer_y_ = static_cast<float>(wl_fixed_to_double(y));
    self.emit_pointer(
        {.kind = PointerEvent::Kind::motion, .x = self.pointer_x_, .y = self.pointer_y_});
}

void Seat::on_pointer_button(void* data, wl_pointer* /*pointer*/, std::uint32_t serial,
                             std::uint32_t time, std::uint32_t button, std::uint32_t state) {
    auto& self = *static_cast<Seat*>(data);
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        self.input_serial_ = serial;
    }
    PointerEvent event{.kind = PointerEvent::Kind::release,
                       .x = self.pointer_x_,
                       .y = self.pointer_y_,
                       .button = to_button(button)};
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        event.kind = PointerEvent::Kind::press;
        LastPress& last = self.last_press_;
        const bool same_click = last.count > 0 && last.button == event.button &&
                                time - last.time_ms <= self.multi_click_ms_ &&
                                std::abs(event.x - last.x) <= self.multi_click_slop_ &&
                                std::abs(event.y - last.y) <= self.multi_click_slop_;
        last = {.button = event.button,
                .time_ms = time,
                .x = event.x,
                .y = event.y,
                .count = same_click ? last.count + 1 : 1};
        event.click_count = last.count;
    }
    self.emit_pointer(event);
}

void Seat::on_pointer_axis(void* data, wl_pointer* /*pointer*/, std::uint32_t /*time*/,
                           std::uint32_t axis, wl_fixed_t value) {
    auto& self = *static_cast<Seat*>(data);
    const auto amount = static_cast<float>(wl_fixed_to_double(value));
    self.pending_scroll_.any = true;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        self.pending_scroll_.y += amount;
    } else {
        self.pending_scroll_.x += amount;
    }
}

void Seat::on_pointer_axis_source(void* data, wl_pointer* /*pointer*/, std::uint32_t source) {
    auto& self = *static_cast<Seat*>(data);
    self.pending_scroll_.finger = source == WL_POINTER_AXIS_SOURCE_FINGER;
}

void Seat::on_pointer_axis_stop(void* data, wl_pointer* /*pointer*/, std::uint32_t /*time*/,
                                std::uint32_t /*axis*/) {
    auto& self = *static_cast<Seat*>(data);
    self.pending_scroll_.any = true;
    self.pending_scroll_.stop = true;
}

void Seat::on_pointer_axis_discrete(void* /*data*/, wl_pointer* /*pointer*/, std::uint32_t /*axis*/,
                                    std::int32_t /*discrete*/) {
    // Not sent to v8 clients: axis_value120 replaces it.
}

void Seat::on_pointer_axis_value120(void* data, wl_pointer* /*pointer*/, std::uint32_t axis,
                                    std::int32_t value120) {
    auto& self = *static_cast<Seat*>(data);
    const float notches = static_cast<float>(value120) / kValue120PerNotch;
    self.pending_scroll_.any = true;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
        self.pending_scroll_.wheel_y += notches;
    } else {
        self.pending_scroll_.wheel_x += notches;
    }
}

void Seat::on_pointer_frame(void* data, wl_pointer* /*pointer*/) {
    auto& self = *static_cast<Seat*>(data);
    const PendingScroll scroll = std::exchange(self.pending_scroll_, {});
    if (!scroll.any) {
        return;
    }
    self.emit_pointer({.kind = PointerEvent::Kind::scroll,
                       .x = self.pointer_x_,
                       .y = self.pointer_y_,
                       .scroll_x = scroll.x,
                       .scroll_y = scroll.y,
                       .wheel_x = scroll.wheel_x,
                       .wheel_y = scroll.wheel_y,
                       .scroll_finger = scroll.finger,
                       .scroll_stop = scroll.stop});
}

void Seat::on_name(void* /*data*/, wl_seat* /*seat*/, const char* name) {
    log::debug("seat: {}", name);
}

void Seat::on_keymap(void* data, wl_keyboard* /*keyboard*/, std::uint32_t format, std::int32_t fd,
                     std::uint32_t size) {
    auto& self = *static_cast<Seat*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        log::warn("unsupported keymap format {}", format);
        close(fd);
        return;
    }
    // Since wl_seat v7 the fd must be mapped MAP_PRIVATE.
    void* map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        log::error("cannot map the keymap: {}", std::strerror(errno));
        return;
    }
    XkbPtr<xkb_keymap, xkb_keymap_unref> keymap{
        xkb_keymap_new_from_string(self.context_.get(), static_cast<const char*>(map),
                                   XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS)};
    munmap(map, size);
    if (!keymap) {
        log::error("cannot compile the compositor's keymap");
        return;
    }
    self.state_.reset(xkb_state_new(keymap.get()));
    self.keymap_ = std::move(keymap);
    log::debug("keymap: layout '{}'", xkb_keymap_layout_get_name(self.keymap_.get(), 0));
}

void Seat::on_keyboard_enter(void* /*data*/, wl_keyboard* /*keyboard*/, std::uint32_t /*serial*/,
                             wl_surface* /*surface*/, wl_array* /*keys*/) {}

void Seat::on_keyboard_leave(void* data, wl_keyboard* /*keyboard*/, std::uint32_t /*serial*/,
                             wl_surface* /*surface*/) {
    auto& self = *static_cast<Seat*>(data);
    self.disarm_repeat();
    if (self.compose_state_) {
        xkb_compose_state_reset(self.compose_state_.get());
    }
}

void Seat::on_key(void* data, wl_keyboard* /*keyboard*/, std::uint32_t serial,
                  std::uint32_t /*time*/, std::uint32_t key, std::uint32_t state) {
    auto& self = *static_cast<Seat*>(data);
    if (!self.state_) {
        return;
    }
    const xkb_keycode_t keycode = key + kEvdevOffset;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (keycode == self.repeat_key_) {
            self.disarm_repeat();
        }
        return;
    }
    self.input_serial_ = serial;
    self.press(keycode, false);
    if (xkb_keymap_key_repeats(self.keymap_.get(), keycode) != 0) {
        self.arm_repeat(keycode);
    } else {
        self.disarm_repeat();
    }
}

void Seat::on_modifiers(void* data, wl_keyboard* /*keyboard*/, std::uint32_t /*serial*/,
                        std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
                        std::uint32_t group) {
    auto& self = *static_cast<Seat*>(data);
    if (self.state_) {
        xkb_state_update_mask(self.state_.get(), depressed, latched, locked, 0, 0, group);
    }
}

void Seat::on_repeat_info(void* data, wl_keyboard* /*keyboard*/, std::int32_t rate,
                          std::int32_t delay) {
    auto& self = *static_cast<Seat*>(data);
    self.repeat_rate_ = rate;
    self.repeat_delay_ = delay;
    log::debug("key repeat: {} per second after {} ms", rate, delay);
}

void Seat::press(xkb_keycode_t keycode, bool repeat) {
    KeyEvent event{.keysym = xkb_state_key_get_one_sym(state_.get(), keycode),
                   .text = {},
                   .modifiers = current_modifiers(),
                   .repeat = repeat};

    if (compose_state_ && !repeat) {
        xkb_compose_state_feed(compose_state_.get(), event.keysym);
    }
    const auto status =
        compose_state_ ? xkb_compose_state_get_status(compose_state_.get()) : XKB_COMPOSE_NOTHING;
    std::array<char, 64> buffer{};
    switch (status) {
        case XKB_COMPOSE_COMPOSING:
        case XKB_COMPOSE_CANCELLED:
            return;  // inside a dead-key sequence, or it was aborted: no key for the app
        case XKB_COMPOSE_COMPOSED:
            event.keysym = xkb_compose_state_get_one_sym(compose_state_.get());
            xkb_compose_state_get_utf8(compose_state_.get(), buffer.data(), buffer.size());
            xkb_compose_state_reset(compose_state_.get());
            break;
        case XKB_COMPOSE_NOTHING:
            xkb_state_key_get_utf8(state_.get(), keycode, buffer.data(), buffer.size());
            break;
    }
    event.text = buffer.data();
    // Control characters (Ctrl+letter, Escape, Backspace...) are not text.
    if (event.text.size() == 1 &&
        (static_cast<unsigned char>(event.text[0]) < 0x20U || event.text[0] == '\x7f')) {
        event.text.clear();
    }
    if (key_handler_) {
        key_handler_(event);
    }
}

void Seat::arm_repeat(xkb_keycode_t keycode) {
    if (repeat_rate_ <= 0) {
        return;
    }
    repeat_key_ = keycode;
    const itimerspec timer = milliseconds_to_timer(repeat_delay_, std::max(1, 1000 / repeat_rate_));
    timerfd_settime(repeat_fd_, 0, &timer, nullptr);
}

void Seat::disarm_repeat() {
    repeat_key_ = 0;
    const itimerspec timer{};
    timerfd_settime(repeat_fd_, 0, &timer, nullptr);
}

void Seat::dispatch_repeat() {
    std::uint64_t expirations = 0;
    if (read(repeat_fd_, &expirations, sizeof(expirations)) != sizeof(expirations) ||
        repeat_key_ == 0) {
        return;
    }
    for (std::uint64_t i = 0; i < std::min(expirations, kMaxRepeatsPerWake); ++i) {
        press(repeat_key_, true);
    }
}

Modifiers Seat::current_modifiers() const {
    const auto active = [&](const char* name) {
        return xkb_state_mod_name_is_active(state_.get(), name, XKB_STATE_MODS_EFFECTIVE) > 0;
    };
    return {.ctrl = active(XKB_MOD_NAME_CTRL),
            .shift = active(XKB_MOD_NAME_SHIFT),
            .alt = active(XKB_MOD_NAME_ALT),
            .super = active(XKB_MOD_NAME_LOGO)};
}

}  // namespace rummage::ui
