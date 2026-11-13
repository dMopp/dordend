#include "rummage/ui/activation.hpp"

#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::ui {

Activation::Activation(Display& display, core::EventLoop& loop) : display_{display}, loop_{loop} {}

Activation::~Activation() {
    for (Pending& pending : pending_) {
        loop_.remove(pending.timer);
    }
}

void Activation::request(wl_seat* seat, std::uint32_t serial, wl_surface* surface,
                         std::function<void(std::string token)> done) {
    if (display_.activation() == nullptr) {
        done({});
        return;
    }
    Pending& pending = pending_.emplace_back();
    pending.owner = this;
    pending.done = std::move(done);
    pending.token.reset(xdg_activation_v1_get_activation_token(display_.activation()));
    static constexpr xdg_activation_token_v1_listener kListener{.done = &Activation::on_done};
    xdg_activation_token_v1_add_listener(pending.token.get(), &kListener, &pending);
    if (seat != nullptr && serial != 0) {
        xdg_activation_token_v1_set_serial(pending.token.get(), serial, seat);
    }
    if (surface != nullptr) {
        xdg_activation_token_v1_set_surface(pending.token.get(), surface);
    }
    xdg_activation_token_v1_commit(pending.token.get());
    pending.timer = loop_.add_timer(kTimeout, std::chrono::milliseconds{0}, [this, &pending] {
        log::warn("activation: no token from the compositor in {} ms", kTimeout.count());
        pending.timer = 0;  // a one-shot timer removes itself
        finish(&pending, {});
    });
}

void Activation::on_done(void* data, xdg_activation_token_v1* /*token*/, const char* name) {
    auto* const pending = static_cast<Pending*>(data);
    pending->owner->finish(pending, name != nullptr ? name : "");
}

void Activation::finish(Pending* pending, std::string token) {
    if (pending->timer != 0) {
        loop_.remove(pending->timer);
    }
    std::function<void(std::string)> done = std::move(pending->done);
    pending_.remove_if([pending](const Pending& item) { return &item == pending; });
    done(std::move(token));
}

}  // namespace rummage::ui
