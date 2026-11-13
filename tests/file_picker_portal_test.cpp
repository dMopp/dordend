#include "rummage/platform/file_picker_portal.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <sdbus-c++/sdbus-c++.h>
#include <thread>
#include <unistd.h>

#include "rummage/core/file_picker.hpp"

namespace {
using namespace std::chrono_literals;
using Values = std::map<std::string, sdbus::Variant>;
using Response = std::pair<std::uint32_t, Values>;
const sdbus::ServiceName service{"org.freedesktop.impl.portal.desktop.rummage"};
const sdbus::ObjectPath path{"/org/freedesktop/portal/desktop"};
constexpr auto iface = "org.freedesktop.impl.portal.FileChooser";

void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error{message};
    }
}

int child() {
    auto request = rummage::core::read_picker_request(3);
    if (request.title == "crash") {
        return 1;
    }
    if (request.title == "wait") {
        for (;;) {
            pause();
        }
    }
    if (request.title == "malformed") {
        return 0;
    }
    rummage::core::PickerResult result;
    if (request.title != "cancel") {
        result.response = 0;
        result.paths = {"/tmp/picker space#\n.txt"};
        if (request.multiple || request.mode == rummage::core::PickerRequest::Mode::save_files) {
            result.paths.push_back("/tmp/second.txt");
        }
        result.filter = request.filter;
        for (const auto& c : request.choices) {
            result.choices.push_back(c.selected);
        }
    }
    rummage::core::write_picker_result(4, result);
    return 0;
}

std::future<Response> call(sdbus::IProxy& proxy, const std::string& method,
                           const std::string& handle, const std::string& title,
                           Values values = {}) {
    auto promise = std::make_shared<std::promise<Response>>();
    auto future = promise->get_future();
    proxy.callMethodAsync(method)
        .onInterface(iface)
        .withArguments(sdbus::ObjectPath{handle}, std::string{"test"}, std::string{}, title, values)
        .uponReplyInvoke(
            [promise](std::optional<sdbus::Error> error, std::uint32_t response, Values result) {
                if (error) {
                    promise->set_exception(std::make_exception_ptr(*error));
                } else {
                    promise->set_value({response, std::move(result)});
                }
            });
    return future;
}

Response get(std::future<Response>& future) {
    require(future.wait_for(5s) == std::future_status::ready, "Portal response timed out");
    return future.get();
}

void client() {
    auto connection = sdbus::createSessionBusConnection();
    connection->enterEventLoopAsync();
    auto proxy = sdbus::createProxy(*connection, service, path);
    bool ready = false;
    for (int i = 0; i < 100 && !ready; ++i) {
        try {
            ready = proxy->getProperty("version").onInterface(iface).get<std::uint32_t>() == 4;
        } catch (const sdbus::Error&) {
            std::this_thread::sleep_for(10ms);
        }
    }
    require(ready, "Backend did not acquire its bus name");
    using Pattern = sdbus::Struct<std::uint32_t, std::string>;
    using Filter = sdbus::Struct<std::string, std::vector<Pattern>>;
    using Option = sdbus::Struct<std::string, std::string>;
    using Choice = sdbus::Struct<std::string, std::string, std::vector<Option>, std::string>;
    Filter filter{"Text", std::vector<Pattern>{{0, "*.txt"}}};
    Values options{
        {"multiple", sdbus::Variant{true}},
        {"current_filter", sdbus::Variant{filter}},
        {"choices", sdbus::Variant{std::vector<Choice>{{"readonly", "Read only", {}, "true"}}}}};
    auto open = call(*proxy, "OpenFile", "/test/open", "open", options);
    auto [status, values] = get(open);
    require(status == 0, "OpenFile failed");
    const auto uris = values.at("uris").get<std::vector<std::string>>();
    require(uris == std::vector<std::string>{"file:///tmp/picker%20space%23%0A.txt",
                                             "file:///tmp/second.txt"},
            "Wrong or unescaped URIs");
    require(values.at("current_filter").get<Filter>() == filter, "Filter not returned");
    require(values.at("choices").get<std::vector<Option>>() ==
                std::vector<Option>{{"readonly", "true"}},
            "Choices not returned");
    auto save = call(*proxy, "SaveFile", "/test/save", "save");
    require(get(save).first == 0, "SaveFile failed");
    Values batch{
        {"files", sdbus::Variant{std::vector<std::vector<std::uint8_t>>{{'a', 0}, {'b', 0}}}}};
    auto saves = call(*proxy, "SaveFiles", "/test/saves", "save", batch);
    require(get(saves).first == 0, "SaveFiles failed");
    auto cancel = call(*proxy, "OpenFile", "/test/cancel", "cancel");
    require(get(cancel).first == 1, "Cancel reported success");
    for (const auto* title : {"crash", "malformed"}) {
        auto failed = call(*proxy, "OpenFile", "/test/failed", title);
        require(get(failed).first == 2, "Child failure did not report an error");
    }
    Values invalid{{"current_folder", sdbus::Variant{std::vector<std::uint8_t>{'x'}}}};
    auto bad = call(*proxy, "OpenFile", "/test/bad", "open", invalid);
    require(get(bad).first == 2, "Unterminated path accepted");
    auto waiting = call(*proxy, "OpenFile", "/test/waiting", "wait");
    auto concurrent = call(*proxy, "OpenFile", "/test/concurrent", "open");
    require(get(concurrent).first == 0, "Blocked picker stopped concurrent requests");
    auto closer = sdbus::createProxy(*connection, service, sdbus::ObjectPath{"/test/waiting"});
    closer->callMethod("Close").onInterface("org.freedesktop.impl.portal.Request");
    require(get(waiting).first == 1, "Request.Close did not cancel");
    bool removed = false;
    try {
        closer->callMethod("Close").onInterface("org.freedesktop.impl.portal.Request");
    } catch (const sdbus::Error&) {
        removed = true;
    }
    require(removed, "Finished Request object was not removed");
    std::cout << "FileChooser D-Bus integration passed\n";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--picker-channel") {
        return child();
    }
    rummage::core::EventLoop loop;
    std::exception_ptr error;
    std::exception_ptr backend_error;
    std::thread caller{[&] {
        try {
            client();
        } catch (...) {
            error = std::current_exception();
        }
        loop.post([&] { loop.quit(); });
    }};
    try {
        rummage::platform::run_file_picker_portal(loop,
                                                  std::filesystem::read_symlink("/proc/self/exe"));
    } catch (...) {
        backend_error = std::current_exception();
    }
    caller.join();
    if (backend_error) {
        error = backend_error;
    }
    if (error) {
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
        }
        return 1;
    }
    return 0;
}
