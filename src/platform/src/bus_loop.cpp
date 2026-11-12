#include "nordend/platform/bus_loop.hpp"

#include <chrono>
#include <poll.h>

#include "nordend/core/log.hpp"

namespace nordend::platform {

BusLoop::BusLoop(core::EventLoop& loop, sdbus::IConnection& connection, const char* name)
    : loop_{loop}, connection_{connection}, name_{name} {}

BusLoop::~BusLoop() {
    loop_.remove(fd_source_);
    loop_.remove(event_source_);
    loop_.remove(timeout_);
}

void BusLoop::pump() {
    if (pumping_) {
        return;  // a callback inside processPendingEvent asked again
    }
    pumping_ = true;
    try {
        while (connection_.processPendingEvent()) {
        }
    } catch (const sdbus::Error& error) {
        log::warn("{}: {}", name_, error.getMessage());
    }
    pumping_ = false;
    watch();
}

void BusLoop::watch() {
    const sdbus::IConnection::PollData data = connection_.getEventLoopPollData();
    if (data.fd != fd_ || data.events != events_ || fd_source_ == 0) {
        loop_.remove(fd_source_);
        fd_ = data.fd;
        events_ = data.events;
        fd_source_ = loop_.add_fd(fd_, events_, [this](short /*revents*/) { pump(); });
    }
    if (event_source_ == 0 && data.eventFd >= 0) {
        event_source_ = loop_.add_fd(data.eventFd, POLLIN, [this](short /*revents*/) { pump(); });
    }
    loop_.remove(timeout_);
    timeout_ = 0;
    const std::chrono::microseconds wait = data.getRelativeTimeout();
    if (wait != std::chrono::microseconds::max()) {
        timeout_ = loop_.add_timer(std::chrono::ceil<std::chrono::milliseconds>(wait),
                                   std::chrono::milliseconds{0}, [this] {
                                       timeout_ = 0;
                                       pump();
                                   });
    }
}

}  // namespace nordend::platform
