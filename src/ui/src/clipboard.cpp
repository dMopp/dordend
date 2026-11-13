#include "rummage/ui/clipboard.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <string_view>
#include <unistd.h>

#include "rummage/core/log.hpp"

namespace rummage::ui {

Clipboard::Clipboard(Display& display, core::EventLoop& loop) : display_{display}, loop_{loop} {
    if (display.data_device_manager() == nullptr || display.seat() == nullptr) {
        log::warn("compositor offers no clipboard: copy and paste stay inside rummage");
        return;
    }
    static constexpr wl_data_device_listener kDeviceListener{
        .data_offer = &Clipboard::on_data_offer,
        .enter = &Clipboard::on_enter,
        .leave = &Clipboard::on_leave,
        .motion = &Clipboard::on_motion,
        .drop = &Clipboard::on_drop,
        .selection = &Clipboard::on_selection,
    };
    device_.reset(
        wl_data_device_manager_get_data_device(display.data_device_manager(), display.seat()));
    wl_data_device_add_listener(device_.get(), &kDeviceListener, this);
}

Clipboard::~Clipboard() {
    for (const Writer& writer : writers_) {
        loop_.remove(writer.source);
        close(writer.fd);
    }
    for (const Reader& reader : readers_) {
        loop_.remove(reader.source);
        loop_.remove(reader.timer);
        close(reader.fd);
    }
    for (const auto& [offer, state] : offers_) {
        wl_data_offer_destroy(offer);
    }
}

void Clipboard::start_drag(std::vector<Format> formats, wl_surface* origin, wl_surface* icon,
                           std::uint32_t serial) {
    if (!device_) {
        return;
    }
    static constexpr wl_data_source_listener kDragListener{
        .target = [](void*, wl_data_source*, const char*) {},
        .send = &Clipboard::on_source_send,
        .cancelled = &Clipboard::on_source_cancelled,
        .dnd_drop_performed = [](void*, wl_data_source*) {},
        .dnd_finished = &Clipboard::on_source_dnd_finished,
        .action = [](void*, wl_data_source*, std::uint32_t) {},
    };
    drag_source_.reset(wl_data_device_manager_create_data_source(display_.data_device_manager()));
    wl_data_source_add_listener(drag_source_.get(), &kDragListener, this);
    for (const Format& format : formats) {
        wl_data_source_offer(drag_source_.get(), format.first.c_str());
    }
    wl_data_source_set_actions(drag_source_.get(), kDndCopy);
    drag_formats_ = std::move(formats);
    wl_data_device_start_drag(device_.get(), drag_source_.get(), origin, icon, serial);
}

void Clipboard::cancel_drag() {
    if (drag_source_) {
        end_drag(false);  // a source destroyed mid-drag ends the drag
    }
}

void Clipboard::end_drag(bool dropped) {
    drag_source_.reset();
    drag_formats_.clear();
    if (drag_ended_) {
        drag_ended_(dropped);
    }
}

const Clipboard::Offer* Clipboard::drop_offer() const {
    wl_data_offer* const offer = dragged_ != nullptr ? dragged_ : dropped_;
    const auto it = offers_.find(offer);
    return it == offers_.end() ? nullptr : &it->second;
}

std::vector<std::string> Clipboard::drop_types() const {
    const Offer* const offer = drop_offer();
    return offer != nullptr ? offer->types : std::vector<std::string>{};
}

std::uint32_t Clipboard::drop_source_actions() const {
    const Offer* const offer = drop_offer();
    return offer != nullptr ? offer->source_actions : 0;
}

std::uint32_t Clipboard::drop_action() const {
    const Offer* const offer = drop_offer();
    return offer != nullptr ? offer->action : 0;
}

void Clipboard::accept_drop(const std::optional<std::string>& type, std::uint32_t actions,
                            std::uint32_t preferred) {
    if (dragged_ == nullptr) {
        return;
    }
    wl_data_offer_accept(dragged_, enter_serial_, type ? type->c_str() : nullptr);
    if (!type || (preferred & actions) == 0) {
        actions = 0;  // the preferred action must be one of them
        preferred = 0;
    }
    wl_data_offer_set_actions(dragged_, actions, preferred);
}

void Clipboard::read_drop(const std::string& type, Done done) {
    wl_data_offer* const offer = dropped_;
    if (offer == nullptr) {
        loop_.post([done = std::move(done)] { done(std::nullopt); });
        return;
    }
    read_offer(offer, type, [this, offer, done = std::move(done)](std::optional<std::string> data) {
        if (dropped_ == offer) {
            finish_drop();
        }
        done(std::move(data));
    });
}

void Clipboard::finish_drop() {
    if (dropped_ == nullptr) {
        return;
    }
    if (const auto it = offers_.find(dropped_); it != offers_.end() && it->second.action != 0) {
        wl_data_offer_finish(dropped_);
    }
    if (dropped_ != selection_) {
        forget_offer(dropped_);
    }
    dropped_ = nullptr;
}

void Clipboard::offer(std::vector<Format> formats, std::uint32_t serial) {
    if (!device_) {
        return;
    }
    static constexpr wl_data_source_listener kSourceListener{
        .target = [](void*, wl_data_source*, const char*) {},
        .send = &Clipboard::on_source_send,
        .cancelled = &Clipboard::on_source_cancelled,
        .dnd_drop_performed = [](void*, wl_data_source*) {},
        .dnd_finished = [](void*, wl_data_source*) {},
        .action = [](void*, wl_data_source*, std::uint32_t) {},
    };
    WlPtr<wl_data_source> source{
        wl_data_device_manager_create_data_source(display_.data_device_manager())};
    wl_data_source_add_listener(source.get(), &kSourceListener, this);
    for (const Format& format : formats) {
        wl_data_source_offer(source.get(), format.first.c_str());
    }
    wl_data_device_set_selection(device_.get(), source.get(), serial);
    source_ = std::move(source);  // the old one goes: no cancelled event for it, nor lost_
    formats_ = std::move(formats);
}

void Clipboard::clear(std::uint32_t serial) {
    if (!device_) {
        return;
    }
    wl_data_device_set_selection(device_.get(), nullptr, serial);
    source_.reset();
    formats_.clear();
}

std::vector<std::string> Clipboard::types() const {
    if (selection_ == nullptr) {
        return {};
    }
    const auto it = offers_.find(selection_);
    return it == offers_.end() ? std::vector<std::string>{} : it->second.types;
}

void Clipboard::read(const std::string& type, Done done) {
    read_offer(selection_, type, std::move(done));
}

void Clipboard::read_offer(wl_data_offer* offer, const std::string& type, Done done) {
    std::array<int, 2> fds{-1, -1};
    if (offer == nullptr || pipe2(fds.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        loop_.post([done = std::move(done)] { done(std::nullopt); });
        return;
    }
    wl_data_offer_receive(offer, type.c_str(), fds[1]);
    wl_display_flush(display_.native());
    close(fds[1]);
    readers_.push_back(
        {.fd = fds[0], .data = {}, .done = std::move(done), .source = 0, .timer = 0});
    const auto reader = std::prev(readers_.end());
    reader->source = loop_.add_fd(reader->fd, POLLIN, [this, reader](short /*revents*/) {
        std::array<char, 65536> buffer{};
        for (;;) {
            const ssize_t got = ::read(reader->fd, buffer.data(), buffer.size());
            if (got > 0) {
                reader->data.append(buffer.data(), static_cast<std::size_t>(got));
                if (reader->data.size() > kMaxRead) {
                    log::warn("clipboard: more than {} bytes, given up", kMaxRead);
                    finish_read(reader, std::nullopt);
                    return;
                }
                continue;
            }
            if (got == 0) {
                finish_read(reader, std::move(reader->data));
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN) {
                finish_read(reader, std::nullopt);
            }
            return;
        }
    });
    reader->timer = loop_.add_timer(kReadTimeout, std::chrono::milliseconds{0}, [this, reader] {
        reader->timer = 0;  // a one-shot timer is gone once it fired
        log::warn("clipboard: the program offering it did not answer in time");
        finish_read(reader, std::nullopt);
    });
}

void Clipboard::finish_read(std::list<Reader>::iterator reader, std::optional<std::string> data) {
    loop_.remove(reader->source);
    if (reader->timer != 0) {
        loop_.remove(reader->timer);
    }
    close(reader->fd);
    Done done = std::move(reader->done);
    readers_.erase(reader);
    done(std::move(data));
}

void Clipboard::write_some(std::list<Writer>::iterator writer) {
    while (writer->written < writer->data.size()) {
        const ssize_t put = ::write(writer->fd, writer->data.data() + writer->written,
                                    writer->data.size() - writer->written);
        if (put > 0) {
            writer->written += static_cast<std::size_t>(put);
            continue;
        }
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put < 0 && errno == EAGAIN) {
            return;  // the pipe is full: when the reader takes some
        }
        break;  // EPIPE: the reader went away
    }
    loop_.remove(writer->source);
    close(writer->fd);
    writers_.erase(writer);
}

void Clipboard::forget_offer(wl_data_offer* offer) {
    if (offer == nullptr) {
        return;
    }
    if (offers_.erase(offer) > 0) {
        wl_data_offer_destroy(offer);
    }
}

void Clipboard::on_data_offer(void* data, wl_data_device* /*device*/, wl_data_offer* offer) {
    auto& self = *static_cast<Clipboard*>(data);
    static constexpr wl_data_offer_listener kOfferListener{
        .offer = &Clipboard::on_offer_type,
        .source_actions = &Clipboard::on_offer_source_actions,
        .action = &Clipboard::on_offer_action,
    };
    self.offers_.emplace(offer, Offer{});
    wl_data_offer_add_listener(offer, &kOfferListener, &self);
}

void Clipboard::on_offer_type(void* data, wl_data_offer* offer, const char* type) {
    auto& self = *static_cast<Clipboard*>(data);
    if (const auto it = self.offers_.find(offer); it != self.offers_.end()) {
        it->second.types.emplace_back(type);
    }
}

void Clipboard::on_offer_source_actions(void* data, wl_data_offer* offer, std::uint32_t actions) {
    auto& self = *static_cast<Clipboard*>(data);
    if (const auto it = self.offers_.find(offer); it != self.offers_.end()) {
        it->second.source_actions = actions;
    }
}

void Clipboard::on_offer_action(void* data, wl_data_offer* offer, std::uint32_t action) {
    auto& self = *static_cast<Clipboard*>(data);
    if (const auto it = self.offers_.find(offer); it != self.offers_.end()) {
        it->second.action = action;
    }
}

void Clipboard::on_enter(void* data, wl_data_device* /*device*/, std::uint32_t serial,
                         wl_surface* /*surface*/, wl_fixed_t x, wl_fixed_t y,
                         wl_data_offer* offer) {
    auto& self = *static_cast<Clipboard*>(data);
    if (self.dragged_ != self.selection_) {
        self.forget_offer(self.dragged_);
    }
    self.dragged_ = offer;
    self.enter_serial_ = serial;
    self.drag_x_ = static_cast<float>(wl_fixed_to_double(x));
    self.drag_y_ = static_cast<float>(wl_fixed_to_double(y));
    if (offer != nullptr && self.drop_handler_) {
        self.drop_handler_({.kind = DropEvent::Kind::enter, .x = self.drag_x_, .y = self.drag_y_});
    }
}

void Clipboard::on_leave(void* data, wl_data_device* /*device*/) {
    auto& self = *static_cast<Clipboard*>(data);
    if (self.dragged_ == nullptr) {
        return;  // after a drop: that offer stays until it is read
    }
    if (self.dragged_ != self.selection_) {
        self.forget_offer(self.dragged_);
    }
    self.dragged_ = nullptr;
    if (self.drop_handler_) {
        self.drop_handler_({.kind = DropEvent::Kind::leave, .x = self.drag_x_, .y = self.drag_y_});
    }
}

void Clipboard::on_motion(void* data, wl_data_device* /*device*/, std::uint32_t /*time*/,
                          wl_fixed_t x, wl_fixed_t y) {
    auto& self = *static_cast<Clipboard*>(data);
    self.drag_x_ = static_cast<float>(wl_fixed_to_double(x));
    self.drag_y_ = static_cast<float>(wl_fixed_to_double(y));
    if (self.dragged_ != nullptr && self.drop_handler_) {
        self.drop_handler_({.kind = DropEvent::Kind::motion, .x = self.drag_x_, .y = self.drag_y_});
    }
}

void Clipboard::on_drop(void* data, wl_data_device* /*device*/) {
    auto& self = *static_cast<Clipboard*>(data);
    if (self.dragged_ == nullptr) {
        return;
    }
    if (self.dropped_ != nullptr && self.dropped_ != self.selection_) {
        self.forget_offer(self.dropped_);  // an earlier drop never read
    }
    self.dropped_ = self.dragged_;
    self.dragged_ = nullptr;
    if (self.drop_handler_) {
        self.drop_handler_({.kind = DropEvent::Kind::drop, .x = self.drag_x_, .y = self.drag_y_});
    }
}

void Clipboard::on_selection(void* data, wl_data_device* /*device*/, wl_data_offer* offer) {
    auto& self = *static_cast<Clipboard*>(data);
    if (self.selection_ != offer && self.selection_ != self.dragged_ &&
        self.selection_ != self.dropped_) {
        self.forget_offer(self.selection_);
    }
    if (self.selection_ != offer || offer == nullptr) {
        ++self.generation_;
    }
    self.selection_ = offer;
}

void Clipboard::on_source_send(void* data, wl_data_source* source, const char* type, int fd) {
    auto& self = *static_cast<Clipboard*>(data);
    const std::vector<Format>& formats =
        source == self.drag_source_.get() ? self.drag_formats_ : self.formats_;
    const auto format = std::ranges::find(formats, std::string_view{type}, &Format::first);
    if (format == formats.end() || fcntl(fd, F_SETFL, O_NONBLOCK) != 0) {
        close(fd);
        return;
    }
    self.writers_.push_back({.fd = fd, .data = format->second, .written = 0, .source = 0});
    const auto writer = std::prev(self.writers_.end());
    writer->source = self.loop_.add_fd(
        fd, POLLOUT, [&self, writer](short /*revents*/) { self.write_some(writer); });
}

void Clipboard::on_source_dnd_finished(void* data, wl_data_source* source) {
    auto& self = *static_cast<Clipboard*>(data);
    if (source == self.drag_source_.get()) {
        self.end_drag(true);
    }
}

void Clipboard::on_source_cancelled(void* data, wl_data_source* source) {
    auto& self = *static_cast<Clipboard*>(data);
    if (source == self.drag_source_.get()) {
        self.end_drag(false);  // dropped nowhere, refused, or Escape in the compositor
        return;
    }
    if (source != self.source_.get()) {
        return;  // a replaced source is destroyed at once: it never gets here
    }
    self.source_.reset();
    self.formats_.clear();
    if (self.lost_) {
        self.lost_();
    }
}

}  // namespace rummage::ui
