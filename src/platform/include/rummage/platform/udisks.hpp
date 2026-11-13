#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/platform/volumes.hpp"

namespace rummage::platform {

// udisks2 on the system bus (ROADMAP 7.3): the file systems on the disks, kept current as disks
// come and go, and mounting, unmounting and powering them off. Every call is asynchronous: the
// bus's socket is watched by the loop, and answers come back on the loop thread. Without a system
// bus or udisks2, available() is false and nothing else happens.
//
// Loop thread only. Must not outlive the loop.
class Udisks {
public:
    // changed runs when volumes() changed.
    Udisks(core::EventLoop& loop, std::function<void()> changed);
    ~Udisks();

    Udisks(const Udisks&) = delete;
    Udisks& operator=(const Udisks&) = delete;
    Udisks(Udisks&&) = delete;
    Udisks& operator=(Udisks&&) = delete;

    [[nodiscard]] bool available() const noexcept;

    [[nodiscard]] const std::vector<Volume>& volumes() const noexcept;

    // An error is udisks2's message, made readable; empty when it worked.
    using Done = std::function<void(const std::string& error)>;

    // Mounts volume where udisks2 puts it (/run/media/USER/LABEL); done gets that folder.
    void mount(
        const std::string& volume,
        std::function<void(const std::filesystem::path& where, const std::string& error)> done);

    void unmount(const std::string& volume, Done done);

    // Unmounts every file system on volume's drive, then powers the drive off (or ejects its
    // medium): it can be unplugged then.
    void eject(const std::string& volume, const Done& done);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rummage::platform
