#include "rummage/platform/portal.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bus_loop.hpp"
#include "rummage/core/log.hpp"

namespace rummage::platform {
namespace {

constexpr std::string_view kService = "org.freedesktop.portal.Desktop";
constexpr std::string_view kPath = "/org/freedesktop/portal/desktop";
constexpr std::string_view kSettings = "org.freedesktop.portal.Settings";
constexpr std::string_view kNamespace = "org.freedesktop.appearance";
// How long the start waits for the portal: it may still be starting (D-Bus activation). Past
// this, the answer comes later and the colours change then.
constexpr std::chrono::milliseconds kStartWait{300};

using Values = std::map<std::string, sdbus::Variant>;
using Namespaces = std::map<std::string, Values>;

std::optional<std::uint32_t> scheme_of(const sdbus::Variant& value) {
    try {
        return value.get<std::uint32_t>();
    } catch (const sdbus::Error&) {
        return std::nullopt;
    }
}

std::optional<std::array<double, 3>> accent_of(const sdbus::Variant& value) {
    try {
        const auto [r, g, b] = value.get<sdbus::Struct<double, double, double>>();
        return std::array{r, g, b};
    } catch (const sdbus::Error&) {
        return std::nullopt;
    }
}

}  // namespace

Appearance appearance_from(std::optional<std::uint32_t> color_scheme,
                           std::optional<std::array<double, 3>> accent) {
    Appearance appearance;
    appearance.dark = color_scheme == 1;
    if (accent && std::ranges::all_of(*accent, [](double v) { return v >= 0.0 && v <= 1.0; })) {
        appearance.accent = accent;
    }
    return appearance;
}

struct DesktopPortal::Impl {
    std::function<void()> changed;
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IProxy> proxy;
    std::unique_ptr<BusLoop> bus;
    sdbus::Slot signals;

    bool available{false};
    std::optional<std::uint32_t> scheme;
    std::optional<std::array<double, 3>> accent;
    Appearance appearance;

    explicit Impl(std::function<void()> on_change) : changed{std::move(on_change)} {}

    // Takes a value; false when the key is not one of ours.
    bool take(std::string_view key, const sdbus::Variant& value) {
        if (key == "color-scheme") {
            scheme = scheme_of(value);
        } else if (key == "accent-color") {
            accent = accent_of(value);
        } else {
            return false;
        }
        return true;
    }

    // ReadAll's answer: available once the portal has said something about the appearance.
    void take_all(const Namespaces& all) {
        const auto it = all.find(std::string{kNamespace});
        if (it == all.end()) {
            log::info("portal: no appearance settings");
            return;
        }
        bool any = false;
        for (const auto& [key, value] : it->second) {
            any = take(key, value) || any;
        }
        if (!any) {
            log::info("portal: no appearance settings");
            return;
        }
        available = true;
        appearance = appearance_from(scheme, accent);
    }

    void read_later() {
        proxy->callMethodAsync("ReadAll")
            .onInterface(std::string{kSettings})
            .withArguments(std::vector<std::string>{std::string{kNamespace}})
            .uponReplyInvoke([this](std::optional<sdbus::Error> error, const Namespaces& all) {
                if (error) {
                    log::info("portal: {}", error->getMessage());
                    return;
                }
                take_all(all);
                if (available && changed) {
                    changed();
                }
            });
        bus->pump();
    }

    void setting_changed(sdbus::Message& message) {
        std::string space;
        std::string key;
        sdbus::Variant value;
        try {
            message >> space >> key >> value;
        } catch (const sdbus::Error&) {
            return;
        }
        if (space != kNamespace || !take(key, value)) {
            return;
        }
        const Appearance fresh = appearance_from(scheme, accent);
        const bool was_available = std::exchange(available, true);
        if (fresh != appearance || !was_available) {
            appearance = fresh;
            if (changed) {
                changed();
            }
        }
    }
};

DesktopPortal::DesktopPortal(core::EventLoop& loop, std::function<void()> changed)
    : impl_{std::make_unique<Impl>(std::move(changed))} {
    try {
        impl_->connection = sdbus::createSessionBusConnection();
        impl_->bus = std::make_unique<BusLoop>(loop, *impl_->connection, "portal");
        impl_->proxy =
            sdbus::createProxy(*impl_->connection, sdbus::ServiceName{std::string{kService}},
                               sdbus::ObjectPath{std::string{kPath}});
        impl_->signals = impl_->connection->addMatch(
            std::string{"type='signal',interface='"} + std::string{kSettings} +
                "',member='SettingChanged',path='" + std::string{kPath} + "'",
            [this](sdbus::Message message) { impl_->setting_changed(message); },
            sdbus::return_slot);
    } catch (const sdbus::Error& error) {
        log::info("portal: no session bus ({}): the desktop's colours are not followed",
                  error.getMessage());
        impl_->signals = {};
        impl_->proxy.reset();
        impl_->bus.reset();
        impl_->connection.reset();
        return;
    }
    try {
        Namespaces all;
        impl_->proxy->callMethod("ReadAll")
            .onInterface(std::string{kSettings})
            .withTimeout(kStartWait)
            .withArguments(std::vector<std::string>{std::string{kNamespace}})
            .storeResultsTo(all);
        impl_->take_all(all);
    } catch (const sdbus::Error& error) {
        const std::string& name = error.getName();
        if (name.ends_with(".ServiceUnknown") || name.ends_with(".NameHasNoOwner") ||
            name.ends_with(".UnknownMethod") || name.ends_with(".UnknownInterface")) {
            log::info("portal: no desktop portal ({})", error.getMessage());
        } else {
            log::info("portal: {} (waiting for the answer)", error.getMessage());
            impl_->read_later();
        }
    }
    impl_->bus->pump();
}

DesktopPortal::~DesktopPortal() = default;

bool DesktopPortal::available() const noexcept { return impl_->available; }

const Appearance& DesktopPortal::appearance() const noexcept { return impl_->appearance; }

}  // namespace rummage::platform
