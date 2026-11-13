#include "rummage/platform/notifications.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <vector>

#include "bus_loop.hpp"
#include "rummage/core/log.hpp"

namespace rummage::platform {

struct Notifications::Impl {
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IProxy> proxy;
    BusLoop bus;

    explicit Impl(core::EventLoop& loop)
        : connection{sdbus::createSessionBusConnection()},
          proxy{sdbus::createProxy(*connection, sdbus::ServiceName{"org.freedesktop.Notifications"},
                                   sdbus::ObjectPath{"/org/freedesktop/Notifications"})},
          bus{loop, *connection, "notifications"} {}
};

Notifications::Notifications(core::EventLoop& loop) : loop_{loop} {}

Notifications::~Notifications() = default;

void Notifications::send(const std::string& summary, const std::string& body, bool error) {
    try {
        if (!impl_) {
            impl_ = std::make_unique<Impl>(loop_);
        }
        const std::map<std::string, sdbus::Variant> hints{
            {"desktop-entry", sdbus::Variant{std::string{"rummage"}}},
            {"urgency", sdbus::Variant{static_cast<std::uint8_t>(error ? 2 : 1)}}};
        impl_->proxy->callMethodAsync("Notify")
            .onInterface("org.freedesktop.Notifications")
            .withTimeout(std::chrono::seconds{2})
            .withArguments(std::string{"rummage"}, std::uint32_t{0}, std::string{"rummage"},
                           summary, body, std::vector<std::string>{}, hints, std::int32_t{-1})
            .uponReplyInvoke([](std::optional<sdbus::Error> failure, std::uint32_t) {
                if (failure) {
                    log::warn("notifications: {}", failure->getMessage());
                }
            });
        impl_->bus.pump();
    } catch (const sdbus::Error& failure) {
        log::warn("notifications: {}", failure.getMessage());
    }
}

}  // namespace rummage::platform
