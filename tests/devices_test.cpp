#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/platform/udisks.hpp"
#include "rummage/platform/volumes.hpp"
#include "rummage/views/browser.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::fs::Place;
using rummage::platform::UdisksObject;
using rummage::platform::Volume;
using rummage::test::TempDir;
using rummage::views::Browser;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

bool run_until(EventLoop& loop, const std::function<bool()>& done,
               std::chrono::milliseconds limit = 5s) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(limit, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

UdisksObject drive(std::string path, std::string bus, bool removable = false) {
    UdisksObject object;
    object.path = std::move(path);
    object.drive_interface = true;
    object.connection_bus = std::move(bus);
    object.removable = removable;
    object.can_power_off = true;
    return object;
}

UdisksObject partition(std::string path, std::string drive, std::string label,
                       std::vector<fs::path> mounted = {}, bool system = false) {
    UdisksObject object;
    object.path = std::move(path);
    object.block = true;
    object.filesystem = true;
    object.id_usage = "filesystem";
    object.id_label = std::move(label);
    object.drive = std::move(drive);
    object.size = 500'107'862'016;
    object.hint_system = system;
    object.mount_points = std::move(mounted);
    return object;
}

// The author's machine, as udisks2 describes it.
std::vector<UdisksObject> machine() {
    const std::string usb = "/org/freedesktop/UDisks2/drives/JMicron";
    const std::string ssd = "/org/freedesktop/UDisks2/drives/SPCC";
    const std::string nvme = "/org/freedesktop/UDisks2/drives/Samsung";
    std::vector<UdisksObject> objects{
        drive(usb, "usb"),
        drive(ssd, ""),
        drive(nvme, ""),
        partition("/org/freedesktop/UDisks2/block_devices/sdb1", usb, "kinetic"),
        partition("/org/freedesktop/UDisks2/block_devices/sda", ssd, "DATA", {"/run/media/me/DATA"},
                  true),
        partition("/org/freedesktop/UDisks2/block_devices/nvme0n1p2", nvme, "", {}, true),
        partition("/org/freedesktop/UDisks2/block_devices/nvme0n1p3", nvme, "rummage", {"/"}, true),
    };
    // Swap, an encrypted container and a partition to ignore are never shown.
    UdisksObject swap = partition("/org/freedesktop/UDisks2/block_devices/zram0", "/", "");
    swap.id_usage = "other";
    UdisksObject crypto = partition("/org/freedesktop/UDisks2/block_devices/sdc1", usb, "");
    crypto.id_usage = "crypto";
    UdisksObject ignored = partition("/org/freedesktop/UDisks2/block_devices/sdb2", usb, "EFI");
    ignored.hint_ignore = true;
    objects.push_back(swap);
    objects.push_back(crypto);
    objects.push_back(ignored);
    return objects;
}

}  // namespace

TEST_CASE("volumes: the disks worth showing, named and told apart") {
    const std::vector<Volume> volumes = rummage::platform::volumes_from(machine());
    REQUIRE(volumes.size() == 3);
    // By name: the system's own unmounted partition is left out, the mounted ones stay.
    CHECK(volumes[0].name == "DATA");
    CHECK_FALSE(volumes[0].external);
    CHECK(volumes[0].mount_point == fs::path{"/run/media/me/DATA"});
    CHECK(volumes[1].name == "kinetic");
    CHECK(volumes[1].external);
    CHECK_FALSE(volumes[1].mount_point);
    CHECK(volumes[2].name == "rummage");

    CHECK(rummage::platform::size_name(500'107'862'016) == "500 GB Volume");
    CHECK(rummage::platform::size_name(2'000'398'934'016) == "2.0 TB Volume");
    CHECK(rummage::platform::size_name(31'000'000) == "31 MB Volume");
    UdisksObject unnamed = partition("/b/sdd1", "/", "");
    unnamed.size = 64'000'000'000;
    REQUIRE(rummage::platform::volumes_from({unnamed}).size() == 1);
    CHECK(rummage::platform::volumes_from({unnamed}).front().name == "64 GB Volume");
}

TEST_CASE("volumes: the sidebar's devices learn their disks; a disk not mounted is added") {
    const std::vector<Volume> volumes = rummage::platform::volumes_from(machine());
    std::vector<Place> places{
        {.section = Place::Section::places,
         .label = "Home",
         .path = "/home/me",
         .icon = "home",
         .disk = {}},
        {.section = Place::Section::devices,
         .label = "DATA",
         .path = "/run/media/me/DATA",
         .icon = "database",
         .disk = {}},
    };
    places = rummage::platform::with_volumes(std::move(places), volumes);
    REQUIRE(places.size() == 3);
    CHECK(places[0].disk.volume.empty());
    CHECK(places[1].disk.volume == "/org/freedesktop/UDisks2/block_devices/sda");
    CHECK(places[1].disk.mounted);
    CHECK_FALSE(places[1].disk.ejectable);
    CHECK(places[2].label == "kinetic");
    CHECK(places[2].path.empty());
    CHECK(places[2].icon == "usb");
    CHECK_FALSE(places[2].disk.mounted);
    CHECK(places[2].disk.ejectable);
}

TEST_CASE("volumes: ejecting a drive unmounts every mounted file system on it") {
    std::vector<UdisksObject> objects = machine();
    objects.push_back(partition("/org/freedesktop/UDisks2/block_devices/sdb3",
                                "/org/freedesktop/UDisks2/drives/JMicron", "photos",
                                {"/run/media/me/photos"}));
    objects[3].mount_points = {"/run/media/me/kinetic"};
    const std::vector<Volume> volumes = rummage::platform::volumes_from(objects);
    const std::vector<const Volume*> going =
        rummage::platform::on_same_drive(volumes, "/org/freedesktop/UDisks2/block_devices/sdb1");
    REQUIRE(going.size() == 2);
    CHECK(going[0]->name == "kinetic");
    CHECK(going[1]->name == "photos");
    CHECK(rummage::platform::on_same_drive(volumes, "/nothing").empty());
}

namespace {

// A browser with disks of its own.
struct Fixture {
    TempDir dir;
    fs::path disk = dir.path / "media" / "kinetic";
    EventLoop loop;
    rummage::core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    Browser browser{loop,    pool,
                    watcher, rummage::fs::MimeDatabase::load(),
                    [] {},   [](rummage::ui::CursorShape /*shape*/) {}};
    std::vector<std::string> calls;
    std::string answer;  // what the next call answers: empty, "in use", another error

    Fixture() {
        fs::create_directories(disk / "FOTOGRAFIA");
        browser.set_home_trash(dir.path / "Trash");
        browser.set_devices({
            .mount =
                [this](const std::string& volume, rummage::views::Devices::Mounted done) {
                    calls.push_back("mount " + volume);
                    loop.post([this, done = std::move(done)] {
                        done(answer.empty() ? disk : fs::path{}, answer);
                    });
                },
            .unmount =
                [this](const std::string& volume, rummage::views::Devices::Done done) {
                    calls.push_back("unmount " + volume);
                    loop.post([this, done = std::move(done)] { done(answer); });
                },
            .eject =
                [this](const std::string& volume, rummage::views::Devices::Done done) {
                    calls.push_back("eject " + volume);
                    loop.post([this, done = std::move(done)] { done(answer); });
                },
        });
    }

    [[nodiscard]] Place place(bool mounted) const {
        return {.section = Place::Section::devices,
                .label = "kinetic",
                .path = mounted ? disk : fs::path{},
                .icon = "usb",
                .disk = {.volume = "/dev/sdb1", .mounted = mounted, .ejectable = true}};
    }

    bool settle() {
        return run_until(loop, [this] { return !browser.loading(); });
    }

    [[nodiscard]] std::string message() { return browser.status_bar().message(); }

    [[nodiscard]] fs::path shown() { return browser.columns().focused_folder()->path(); }
};

}  // namespace

TEST_CASE("devices: a disk not mounted is mounted when chosen, then opened") {
    Fixture f;
    f.browser.open(f.dir.path);
    REQUIRE(f.settle());
    f.browser.open_place(f.place(false));
    REQUIRE(f.settle());
    CHECK(f.calls == std::vector<std::string>{"mount /dev/sdb1"});
    CHECK(f.shown() == f.disk);
    CHECK(f.message() == "“kinetic” mounted");

    f.answer = "wrong fs type";
    f.browser.mount_place(f.place(false), true);
    REQUIRE(f.settle());
    CHECK(f.message() == "“kinetic” could not be mounted: wrong fs type");
}

TEST_CASE("devices: unmounting and ejecting leave the disk first, and say how it went") {
    Fixture f;
    f.browser.open(f.disk / "FOTOGRAFIA");
    REQUIRE(f.settle());
    f.answer = "in use";
    f.browser.eject_place(f.place(true));
    REQUIRE(f.settle());
    CHECK(f.calls == std::vector<std::string>{"eject /dev/sdb1"});
    CHECK(f.shown() != f.disk / "FOTOGRAFIA");  // gone home before
    CHECK(f.message() == "“kinetic” is in use: close the files open from it");

    f.answer.clear();
    f.browser.eject_place(f.place(true));
    REQUIRE(f.settle());
    CHECK(f.message() == "“kinetic” can be unplugged now");

    f.browser.unmount_place(f.place(true));
    REQUIRE(f.settle());
    CHECK(f.message() == "“kinetic” unmounted");
    f.answer = "not allowed";
    f.browser.unmount_place(f.place(true));
    REQUIRE(f.settle());
    CHECK(f.message() == "“kinetic” could not be unmounted: not allowed");

    // A mounted place opens as any other, without asking udisks2.
    f.calls.clear();
    f.browser.open_place(f.place(true));
    REQUIRE(f.settle());
    CHECK(f.calls.empty());
    CHECK(f.shown() == f.disk);
}

TEST_CASE("devices: udisks2 on the real system bus lists the disks (reads only)") {
    EventLoop loop;
    int changes = 0;
    rummage::platform::Udisks udisks{loop, [&changes] { ++changes; }};
    if (!udisks.available()) {
        MESSAGE("no system bus or udisks2: skipped");
        return;
    }
    // The first answer comes back through the loop; a machine without any disk worth showing
    // never changes, which is fine too.
    run_until(loop, [&] { return changes > 0; }, 2s);
    for (const Volume& volume : udisks.volumes()) {
        CHECK_FALSE(volume.object.empty());
        CHECK_FALSE(volume.name.empty());
        const std::string external = volume.external ? " (external)" : "";
        const std::string where =
            volume.mount_point ? " at " + volume.mount_point->string() : std::string{};
        MESSAGE(volume.name << " " << volume.device << external << where);
    }
}
