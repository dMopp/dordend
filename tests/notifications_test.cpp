#include "rummage/platform/notifications.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <stdexcept>
#include <vector>

#include "bus_loop.hpp"
#include "rummage/core/i18n.hpp"
#include "rummage/platform/action_handoff.hpp"

namespace {
using namespace std::chrono_literals;
using Hints = std::map<std::string, sdbus::Variant>;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

struct Notice {
    std::string app;
    std::uint32_t replaces;
    std::string icon;
    std::string summary;
    std::string body;
    std::vector<std::string> actions;
    Hints hints;
    std::int32_t timeout;
};
}  // namespace

int main() {
    // The checks below compare English texts, whatever the locale of the machine running them.
    rummage::i18n::set_language(rummage::i18n::Language::english);
    try {
        rummage::core::EventLoop loop;
        rummage::platform::Notifications notifications{loop};
        rummage::platform::ActionHandoff handoff{
            loop, rummage::platform::Environment::from_environment(), [](std::string, bool) {}};
        std::vector<Notice> received;
        {
            auto connection = sdbus::createSessionBusConnection();
            connection->requestName(sdbus::ServiceName{"org.freedesktop.Notifications"});
            auto object = sdbus::createObject(*connection,
                                              sdbus::ObjectPath{"/org/freedesktop/Notifications"});
            object
                ->addVTable(sdbus::registerMethod("Notify").implementedAs(
                    [&](const std::string& app, std::uint32_t replaces, const std::string& icon,
                        const std::string& summary, const std::string& body,
                        const std::vector<std::string>& actions, const Hints& hints,
                        std::int32_t timeout) {
                        received.push_back(
                            {app, replaces, icon, summary, body, actions, hints, timeout});
                        if (received.size() == 2) {
                            // Allow the client's asynchronous reply handlers to run too.
                            loop.add_timer(20ms, 0ms, [&] { loop.quit(0); });
                        }
                        return static_cast<std::uint32_t>(received.size());
                    }))
                .forInterface("org.freedesktop.Notifications");
            rummage::platform::BusLoop server{loop, *connection, "test-notifications"};
            server.pump();
            rummage::core::Action action{.id = "compress",
                                         .label = "Compress <files> & folders",
                                         .argv = {"sh", "-c", "exit 0"},
                                         .mime = {},
                                         .selection = rummage::core::Action::Selection::background,
                                         .notify_on_completion = true};
            handoff.run(action, {.directory = "/tmp", .targets = {}});
            action.label = "Compress";
            action.argv.back() = "exit 7";
            handoff.run(action, {.directory = "/tmp", .targets = {}});
            const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
            require(loop.run() == 0, "Notification delivery timed out");
            loop.remove(timeout);
        }
        require(received.size() == 2, "Expected one notification for each completion");
        // The two commands may finish in either order.
        if (received[0].summary.starts_with("Failed:")) {
            std::swap(received[0], received[1]);
        }
        require(received[0].summary == "Completed: Compress <files> & folders",
                "Action label was changed");
        require(received[0].body == "Completed successfully", "Wrong completion message");
        require(received[1].summary == "Failed: Compress", "Failure reported as success");
        require(received[1].body == "Program exited with status 7", "Wrong error message");
        for (std::size_t i = 0; i < received.size(); ++i) {
            const auto& notice = received[i];
            require(notice.app == "rummage" && notice.icon == "rummage", "Wrong app identity");
            require(notice.replaces == 0, "Concurrent notifications must not replace each other");
            require(notice.actions.empty() && notice.timeout == -1, "Wrong notification options");
            require(notice.hints.at("desktop-entry").get<std::string>() == "rummage",
                    "Wrong desktop entry");
            require(notice.hints.at("urgency").get<std::uint8_t>() == (i == 0 ? 1 : 2),
                    "Wrong notification urgency");
        }
        // Losing the service must not throw, block the loop or change an action's result.
        notifications.send("Completed: No service", "Completed successfully");
        loop.add_timer(100ms, 0ms, [&] { loop.quit(0); });
        require(loop.run() == 0, "Missing service stopped the event loop");
        std::cout << "Notification delivery and missing-service checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
