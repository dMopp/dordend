#pragma once

#include <memory>
#include <string>

#include "rummage/core/event_loop.hpp"

namespace rummage::platform {

// Sends desktop notifications asynchronously through the session bus. A missing notification
// service is logged and never changes the result of the operation being reported.
class Notifications {
public:
    explicit Notifications(core::EventLoop& loop);
    ~Notifications();
    void send(const std::string& summary, const std::string& body, bool error = false);

private:
    struct Impl;
    core::EventLoop& loop_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rummage::platform
