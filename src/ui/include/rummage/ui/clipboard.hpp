#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/ui/display.hpp"
#include "rummage/ui/wl_ptr.hpp"

namespace rummage::ui {

// Drag and drop actions, as wl_data_device_manager numbers them (flags).
inline constexpr std::uint32_t kDndCopy = 1U;
inline constexpr std::uint32_t kDndMove = 2U;

// A drag over a rummage surface, as the window sees it.
struct DropEvent {
    enum class Kind : std::uint8_t { enter, motion, leave, drop };

    Kind kind{Kind::motion};
    float x{0.0F};  // surface-local logical pixels; the last position for leave and drop
    float y{0.0F};
};

// The Wayland clipboard (wl_data_device's selection): offering data in several types, and reading
// what another program offers. The same device carries drag and drop: rummage's own drags
// (start_drag) and drags over its window, from itself or another program (the drop handler). Data
// goes through pipes the loop watches: a slow or stuck reader or writer never blocks the window.
// Writes to a reader that went away fail with EPIPE: the app blocks SIGPIPE.
//
// Loop thread only. Must not outlive the Display or the loop. Not copyable or movable: listeners
// keep a pointer to it.
class Clipboard {
public:
    // A read the other program has not finished by then is given up.
    static constexpr std::chrono::milliseconds kReadTimeout{3000};
    static constexpr std::size_t kMaxRead = 64U << 20U;

    using Format = std::pair<std::string, std::string>;  // type, bytes
    using Done = std::function<void(std::optional<std::string> data)>;

    Clipboard(Display& display, core::EventLoop& loop);
    ~Clipboard();

    Clipboard(const Clipboard&) = delete;
    Clipboard& operator=(const Clipboard&) = delete;
    Clipboard(Clipboard&&) = delete;
    Clipboard& operator=(Clipboard&&) = delete;

    // The compositor has a clipboard for this seat (wl_data_device_manager).
    [[nodiscard]] bool available() const noexcept { return device_ != nullptr; }

    // Takes the clipboard with formats. serial: the key or button press that asked (the
    // compositor ignores requests without a recent one).
    void offer(std::vector<Format> formats, std::uint32_t serial);

    // Empties the clipboard, whoever's it was.
    void clear(std::uint32_t serial);

    // rummage's offer is the one on the clipboard.
    [[nodiscard]] bool owned() const noexcept { return source_ != nullptr; }

    // Changes whenever the clipboard gets other contents (anyone's): a later look can tell
    // whether it still holds what was read.
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

    // The types on the clipboard now (rummage's own too); empty when it holds nothing.
    [[nodiscard]] std::vector<std::string> types() const;

    // Reads type from what is on the clipboard; done gets nullopt when there is nothing, the
    // program failed, or after kReadTimeout. Never at once: on a later iteration of the loop.
    void read(const std::string& type, Done done);

    // rummage's offer was replaced: another program copied something (or rummage did again).
    void set_lost_handler(std::function<void()> lost) { lost_ = std::move(lost); }

    // Starts dragging formats from origin, offered for copying only (whoever takes them, rummage
    // never deletes its files for them). serial: the button press that holds the pointer. icon:
    // a surface drawn under the pointer, or nullptr.
    void start_drag(std::vector<Format> formats, wl_surface* origin, wl_surface* icon,
                    std::uint32_t serial);

    // rummage's drag is on (from start_drag until dropped or cancelled).
    [[nodiscard]] bool dragging() const noexcept { return drag_source_ != nullptr; }

    // Ends rummage's drag without a drop (Escape).
    void cancel_drag();

    // rummage's drag ended: dropped (and taken) or not.
    void set_drag_end_handler(std::function<void(bool dropped)> ended) {
        drag_ended_ = std::move(ended);
    }

    // A drag moving over rummage's window: every enter, motion, leave and drop.
    void set_drop_handler(std::function<void(const DropEvent& event)> handler) {
        drop_handler_ = std::move(handler);
    }

    // What the drag over the window carries: its types, the actions its source allows, and the
    // action agreed for the drop (kDndCopy, kDndMove; 0 before one is).
    [[nodiscard]] std::vector<std::string> drop_types() const;
    [[nodiscard]] std::uint32_t drop_source_actions() const;
    [[nodiscard]] std::uint32_t drop_action() const;

    // Where the pointer is now (enter, motion): type is taken there (nullopt: nothing is), with
    // one of actions, preferred the one it would rather.
    void accept_drop(const std::optional<std::string>& type, std::uint32_t actions,
                     std::uint32_t preferred);

    // After a drop: reads type from it, then tells the source it is done (or gives up after
    // kReadTimeout, done gets nullopt). Never at once.
    void read_drop(const std::string& type, Done done);

    // After a drop whose data rummage has already (its own drag): done with it unread.
    void finish_drop();

private:
    struct Writer {
        int fd{-1};
        std::string data;
        std::size_t written{0};
        core::EventLoop::SourceId source{0};
    };

    struct Reader {
        int fd{-1};
        std::string data;
        Done done;
        core::EventLoop::SourceId source{0};
        core::EventLoop::SourceId timer{0};
    };

    static void on_data_offer(void* data, wl_data_device* device, wl_data_offer* offer);
    static void on_enter(void* data, wl_data_device* device, std::uint32_t serial,
                         wl_surface* surface, wl_fixed_t x, wl_fixed_t y, wl_data_offer* offer);
    static void on_leave(void* data, wl_data_device* device);
    static void on_motion(void* data, wl_data_device* device, std::uint32_t time, wl_fixed_t x,
                          wl_fixed_t y);
    static void on_drop(void* data, wl_data_device* device);
    static void on_selection(void* data, wl_data_device* device, wl_data_offer* offer);
    static void on_offer_type(void* data, wl_data_offer* offer, const char* type);
    static void on_source_send(void* data, wl_data_source* source, const char* type, int fd);
    static void on_source_cancelled(void* data, wl_data_source* source);

    static void on_offer_source_actions(void* data, wl_data_offer* offer, std::uint32_t actions);
    static void on_offer_action(void* data, wl_data_offer* offer, std::uint32_t action);
    static void on_source_dnd_finished(void* data, wl_data_source* source);

    // An offer the compositor introduced: its types and, for a drag, its actions.
    struct Offer {
        std::vector<std::string> types;
        std::uint32_t source_actions{0};
        std::uint32_t action{0};
    };

    // Reads type from offer into done (the clipboard's, or a drop's).
    void read_offer(wl_data_offer* offer, const std::string& type, Done done);
    // The drag over the window, or the one just dropped.
    [[nodiscard]] const Offer* drop_offer() const;
    void end_drag(bool dropped);
    void forget_offer(wl_data_offer* offer);
    void write_some(std::list<Writer>::iterator writer);
    void finish_read(std::list<Reader>::iterator reader, std::optional<std::string> data);

    Display& display_;
    core::EventLoop& loop_;
    WlPtr<wl_data_device> device_;
    WlPtr<wl_data_source> source_;  // rummage's offer, while it is on the clipboard
    std::vector<Format> formats_;   // what it offers
    // Every offer the compositor introduced and not yet destroyed.
    std::map<wl_data_offer*, Offer> offers_;
    wl_data_offer* selection_{nullptr};  // the clipboard's, one of offers_
    wl_data_offer* dragged_{nullptr};    // a drag over the window
    wl_data_offer* dropped_{nullptr};    // a drop being read
    std::uint32_t enter_serial_{0};      // of the drag over the window
    float drag_x_{0.0F};                 // where it is
    float drag_y_{0.0F};
    WlPtr<wl_data_source> drag_source_;  // rummage's drag
    std::vector<Format> drag_formats_;
    std::function<void(bool)> drag_ended_;
    std::function<void(const DropEvent&)> drop_handler_;
    std::uint64_t generation_{0};
    std::list<Writer> writers_;  // lists: callbacks keep iterators
    std::list<Reader> readers_;
    std::function<void()> lost_;
};

}  // namespace rummage::ui
