#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "rummage/fs/places.hpp"

// Disks as udisks2 describes them (ROADMAP 7.3), and the sidebar's places they become. Plain data
// and pure functions: Udisks fills them from D-Bus, tests from tables.
namespace rummage::platform {

// One object of udisks2's object manager, with the properties the sidebar needs.
struct UdisksObject {
    std::string path;  // the D-Bus object path

    // org.freedesktop.UDisks2.Block
    bool block{false};
    std::string device;  // "/dev/sdb1"
    std::uint64_t size{0};
    std::string id_usage;  // "filesystem", "crypto", "other"...
    std::string id_label;
    bool hint_ignore{false};
    bool hint_system{false};
    std::string drive;  // the Drive's object path; "/" for none
    bool read_only{false};

    // org.freedesktop.UDisks2.Filesystem
    bool filesystem{false};
    std::vector<std::filesystem::path> mount_points;

    // org.freedesktop.UDisks2.Drive
    bool drive_interface{false};
    bool removable{false};
    bool ejectable{false};
    bool can_power_off{false};
    std::string connection_bus;  // "usb", "sdio", "ieee1394", "" for internal ones
};

// A file system the sidebar can show.
struct Volume {
    std::string object;  // its Block's object path
    std::string device;
    std::string name;      // its label, else "466 GB Volume"
    std::string drive;     // the Drive's object path; "/" for none
    bool external{false};  // the drive can be unplugged (removable, USB, SD, FireWire)
    std::optional<std::filesystem::path> mount_point;  // the first one
};

// "466 GB Volume", "1.0 TB Volume" (decimal units, as GNOME names a disk without a label).
[[nodiscard]] std::string size_name(std::uint64_t bytes);

// The file systems worth showing: a Block with a Filesystem, not to be ignored, whose usage is a
// file system; the system's own (internal, not removable) only while mounted.
[[nodiscard]] std::vector<Volume> volumes_from(const std::vector<UdisksObject>& objects);

// The places with the volumes in: a mounted disk's place (from the mount table) learns its
// volume and whether it ejects; a volume not mounted is added to the devices, without a path.
[[nodiscard]] std::vector<fs::Place> with_volumes(std::vector<fs::Place> places,
                                                  const std::vector<Volume>& volumes);

// The file systems to unmount before the drive of volume can be powered off: every mounted
// volume on the same drive (volume itself alone when it has none).
[[nodiscard]] std::vector<const Volume*> on_same_drive(const std::vector<Volume>& volumes,
                                                       const std::string& volume);

}  // namespace rummage::platform
