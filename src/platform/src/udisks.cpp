#include "rummage/platform/udisks.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string_view>
#include <utility>

#include "bus_loop.hpp"
#include "rummage/core/log.hpp"

namespace rummage::platform {
namespace {

constexpr std::string_view kService = "org.freedesktop.UDisks2";
constexpr std::string_view kManagerPath = "/org/freedesktop/UDisks2";
constexpr std::string_view kBlock = "org.freedesktop.UDisks2.Block";
constexpr std::string_view kFilesystem = "org.freedesktop.UDisks2.Filesystem";
constexpr std::string_view kDrive = "org.freedesktop.UDisks2.Drive";
// Signals come in bursts (a disk plugged in: a dozen objects): read the objects once after.
constexpr std::chrono::milliseconds kSettle{150};

using Properties = std::map<std::string, sdbus::Variant>;
using Interfaces = std::map<std::string, Properties>;
using ManagedObjects = std::map<sdbus::ObjectPath, Interfaces>;

template <typename T>
T property(const Properties& properties, const char* name, T fallback = {}) {
    const auto it = properties.find(name);
    if (it == properties.end()) {
        return fallback;
    }
    try {
        return it->second.get<T>();
    } catch (const sdbus::Error&) {
        return fallback;  // another type than documented
    }
}

// "ay" byte strings (device names, mount points) end with a NUL.
std::string bytes_text(const std::vector<std::uint8_t>& bytes) {
    std::string text(bytes.begin(), bytes.end());
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

UdisksObject object_from(const sdbus::ObjectPath& path, const Interfaces& interfaces) {
    UdisksObject object;
    object.path = path;
    if (const auto block = interfaces.find(std::string{kBlock}); block != interfaces.end()) {
        const Properties& p = block->second;
        object.block = true;
        object.device = bytes_text(property<std::vector<std::uint8_t>>(p, "Device"));
        object.size = property<std::uint64_t>(p, "Size");
        object.id_usage = property<std::string>(p, "IdUsage");
        object.id_label = property<std::string>(p, "IdLabel");
        object.hint_ignore = property<bool>(p, "HintIgnore");
        object.hint_system = property<bool>(p, "HintSystem");
        object.drive = property<sdbus::ObjectPath>(p, "Drive", sdbus::ObjectPath{"/"});
        object.read_only = property<bool>(p, "ReadOnly");
    }
    if (const auto fs = interfaces.find(std::string{kFilesystem}); fs != interfaces.end()) {
        object.filesystem = true;
        for (const auto& point :
             property<std::vector<std::vector<std::uint8_t>>>(fs->second, "MountPoints")) {
            object.mount_points.emplace_back(bytes_text(point));
        }
    }
    if (const auto drive = interfaces.find(std::string{kDrive}); drive != interfaces.end()) {
        const Properties& p = drive->second;
        object.drive_interface = true;
        object.removable = property<bool>(p, "Removable");
        object.ejectable = property<bool>(p, "Ejectable");
        object.can_power_off = property<bool>(p, "CanPowerOff");
        object.connection_bus = property<std::string>(p, "ConnectionBus");
    }
    return object;
}

// udisks2's errors as a person reads them.
std::string readable(const sdbus::Error& error) {
    const std::string& name = error.getName();
    if (name.ends_with(".DeviceBusy")) {
        return "in use";
    }
    if (name.ends_with(".NotAuthorized") || name.ends_with(".NotAuthorizedCanObtain") ||
        name.ends_with(".NotAuthorizedDismissed")) {
        return "not allowed";
    }
    std::string message = error.getMessage();
    // "Error mounting /dev/sdb1 at /run/media/me/x: wrong fs type..." -> after the last ": ".
    if (const std::size_t colon = message.rfind(": "); colon != std::string::npos) {
        message = message.substr(colon + 2);
    }
    return message.empty() ? name : message;
}

}  // namespace

struct Udisks::Impl {
    core::EventLoop& loop;
    std::function<void()> changed;
    std::unique_ptr<sdbus::IConnection> connection;
    std::unique_ptr<sdbus::IProxy> manager;
    std::map<std::string, std::unique_ptr<sdbus::IProxy>> proxies;  // by object path
    sdbus::Slot signals;
    std::vector<UdisksObject> objects;
    std::vector<Volume> volumes;

    std::unique_ptr<BusLoop> bus;
    core::EventLoop::SourceId settle{0};

    Impl(core::EventLoop& event_loop, std::function<void()> on_change)
        : loop{event_loop}, changed{std::move(on_change)} {}

    ~Impl() { loop.remove(settle); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    sdbus::IProxy& proxy(const std::string& path) {
        auto& slot = proxies[path];
        if (!slot) {
            slot = sdbus::createProxy(*connection, sdbus::ServiceName{std::string{kService}},
                                      sdbus::ObjectPath{path});
        }
        return *slot;
    }

    void pump() const {
        if (bus) {
            bus->pump();
        }
    }

    void read_objects() {
        manager->callMethodAsync("GetManagedObjects")
            .onInterface("org.freedesktop.DBus.ObjectManager")
            .uponReplyInvoke([this](std::optional<sdbus::Error> error, const ManagedObjects& all) {
                if (error) {
                    log::warn("udisks: {}", error->getMessage());
                    return;
                }
                objects.clear();
                for (const auto& [path, interfaces] : all) {
                    objects.push_back(object_from(path, interfaces));
                }
                std::vector<Volume> fresh = volumes_from(objects);
                const auto same = [](const Volume& a, const Volume& b) {
                    return a.object == b.object && a.name == b.name &&
                           a.mount_point == b.mount_point && a.external == b.external;
                };
                if (!std::ranges::equal(fresh, volumes, same)) {
                    volumes = std::move(fresh);
                    if (changed) {
                        changed();
                    }
                }
            });
        pump();
    }

    void read_objects_soon() {
        if (settle != 0) {
            return;
        }
        settle = loop.add_timer(kSettle, std::chrono::milliseconds{0}, [this] {
            settle = 0;
            read_objects();
        });
    }

    [[nodiscard]] const UdisksObject* object(const std::string& path) const {
        const auto it = std::ranges::find(objects, path, &UdisksObject::path);
        return it == objects.end() ? nullptr : &*it;
    }
};

Udisks::Udisks(core::EventLoop& loop, std::function<void()> changed)
    : impl_{std::make_unique<Impl>(loop, std::move(changed))} {
    try {
        impl_->connection = sdbus::createSystemBusConnection();
        impl_->bus = std::make_unique<BusLoop>(loop, *impl_->connection, "udisks");
        impl_->manager =
            sdbus::createProxy(*impl_->connection, sdbus::ServiceName{std::string{kService}},
                               sdbus::ObjectPath{std::string{kManagerPath}});
        // Disks plugged in, unplugged, mounted, unmounted: whatever udisks2 says, read again.
        impl_->signals = impl_->connection->addMatch(
            "type='signal',sender='org.freedesktop.UDisks2'",
            [this](const sdbus::Message& /*message*/) { impl_->read_objects_soon(); },
            sdbus::return_slot);
        impl_->read_objects();
    } catch (const sdbus::Error& error) {
        log::warn("udisks: no system bus or udisks2 ({}): disks are not mounted from rummage",
                  error.getMessage());
        impl_->manager.reset();
        impl_->bus.reset();
        impl_->connection.reset();
    }
}

Udisks::~Udisks() = default;

bool Udisks::available() const noexcept { return impl_->connection != nullptr; }

const std::vector<Volume>& Udisks::volumes() const noexcept { return impl_->volumes; }

void Udisks::mount(
    const std::string& volume,
    std::function<void(const std::filesystem::path& where, const std::string& error)> done) {
    if (!available()) {
        done({}, "udisks2 is not running");
        return;
    }
    impl_->proxy(volume)
        .callMethodAsync("Mount")
        .onInterface(std::string{kFilesystem})
        .withArguments(std::map<std::string, sdbus::Variant>{})
        .uponReplyInvoke(
            [done = std::move(done)](std::optional<sdbus::Error> error, const std::string& where) {
                if (error) {
                    done({}, readable(*error));
                } else {
                    done(where, {});
                }
            });
    impl_->pump();
}

void Udisks::unmount(const std::string& volume, Done done) {
    if (!available()) {
        done("udisks2 is not running");
        return;
    }
    impl_->proxy(volume)
        .callMethodAsync("Unmount")
        .onInterface(std::string{kFilesystem})
        .withArguments(std::map<std::string, sdbus::Variant>{})
        .uponReplyInvoke([done = std::move(done)](std::optional<sdbus::Error> error) {
            done(error ? readable(*error) : std::string{});
        });
    impl_->pump();
}

void Udisks::eject(const std::string& volume, const Done& done) {
    if (!available()) {
        done("udisks2 is not running");
        return;
    }
    const auto it = std::ranges::find(impl_->volumes, volume, &Volume::object);
    if (it == impl_->volumes.end()) {
        done("it is gone already");
        return;
    }
    std::vector<std::string> mounted;
    for (const Volume* other : on_same_drive(impl_->volumes, volume)) {
        mounted.push_back(other->object);
    }
    const std::string drive = it->drive;
    // One after the other, then the drive; the first error stops it.
    auto next = std::make_shared<std::function<void(std::size_t)>>();
    *next = [this, mounted, drive, done, next](std::size_t index) {
        if (index < mounted.size()) {
            unmount(mounted[index], [done, next, index](const std::string& error) {
                if (!error.empty()) {
                    done(error);
                    *next = nullptr;  // the cycle through the lambda ends here
                    return;
                }
                (*next)(index + 1);
            });
            return;
        }
        const UdisksObject* const object = impl_->object(drive);
        if (drive == "/" || object == nullptr) {
            done({});  // nothing to power off: unmounted is all
            *next = nullptr;
            return;
        }
        const bool power_off = object->can_power_off;
        impl_->proxy(drive)
            .callMethodAsync(power_off ? "PowerOff" : "Eject")
            .onInterface(std::string{kDrive})
            .withArguments(std::map<std::string, sdbus::Variant>{})
            .uponReplyInvoke([done](std::optional<sdbus::Error> error) {
                done(error ? readable(*error) : std::string{});
            });
        impl_->pump();
        *next = nullptr;
    };
    (*next)(0);
}

}  // namespace rummage::platform
