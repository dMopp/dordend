#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <string>

#include "rummage/core/event_loop.hpp"
#include "rummage/ui/display.hpp"
#include "rummage/ui/wl_ptr.hpp"

namespace rummage::ui {

// xdg-activation-v1 tokens. An application rummage starts finds one in XDG_ACTIVATION_TOKEN and
// hands it back to the compositor with its first window, which may then take the focus: the
// compositor knows the request came from a user's key or click in rummage.
//
// Loop thread only. Must not outlive the Display or the loop. Not copyable or movable: listeners
// keep pointers into it.
class Activation {
public:
    // A token the compositor has not sent by then is given up: the application starts without.
    static constexpr std::chrono::milliseconds kTimeout{500};

    Activation(Display& display, core::EventLoop& loop);
    ~Activation();

    Activation(const Activation&) = delete;
    Activation& operator=(const Activation&) = delete;
    Activation(Activation&&) = delete;
    Activation& operator=(Activation&&) = delete;

    // Asks for a token tied to the input event serial (a key or button press on seat) and to
    // surface, the window it happened in. done gets the token; "" at once without xdg-activation,
    // or after kTimeout.
    void request(wl_seat* seat, std::uint32_t serial, wl_surface* surface,
                 std::function<void(std::string token)> done);

private:
    struct Pending {
        Activation* owner{nullptr};
        WlPtr<xdg_activation_token_v1> token;
        std::function<void(std::string)> done;
        core::EventLoop::SourceId timer{0};
    };

    static void on_done(void* data, xdg_activation_token_v1* token, const char* name);
    void finish(Pending* pending, std::string token);

    Display& display_;
    core::EventLoop& loop_;
    std::list<Pending> pending_;  // a list: listeners point at its elements
};

}  // namespace rummage::ui
