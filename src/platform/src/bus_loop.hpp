#pragma once

#include <sdbus-c++/sdbus-c++.h>

#include "rummage/core/event_loop.hpp"

namespace rummage::platform {

// A D-Bus connection served by the loop instead of a thread of its own: its socket for what it
// asks, its event fd, its timeout. Calls and signals are then handled on the loop thread.
//
// Loop thread only. Must not outlive the loop or the connection.
class BusLoop {
public:
    BusLoop(core::EventLoop& loop, sdbus::IConnection& connection, const char* name);
    ~BusLoop();

    BusLoop(const BusLoop&) = delete;
    BusLoop& operator=(const BusLoop&) = delete;
    BusLoop(BusLoop&&) = delete;
    BusLoop& operator=(BusLoop&&) = delete;

    // Lets the bus read and write what it has, then watches what it waits for next. Call it
    // after making a call.
    void pump();

private:
    void watch();

    core::EventLoop& loop_;
    sdbus::IConnection& connection_;
    const char* name_;  // for log lines
    int fd_{-1};
    short events_{0};
    core::EventLoop::SourceId fd_source_{0};
    core::EventLoop::SourceId event_source_{0};
    core::EventLoop::SourceId timeout_{0};
    bool pumping_{false};
};

}  // namespace rummage::platform
