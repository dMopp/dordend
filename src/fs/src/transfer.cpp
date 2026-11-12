#include "nordend/fs/transfer.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <system_error>
#include <unistd.h>
#include <utility>

#include "nordend/fs/operations.hpp"
#include "nordend/core/i18n.hpp"

namespace nordend::fs {
namespace {

constexpr std::size_t kChunk = std::size_t{8} << 20U;      // bytes per copy_file_range call
constexpr std::size_t kBuffer = std::size_t{1} << 20U;     // read/write fallback
constexpr std::string_view kTemporary = ".nordend-part-";  // hidden, recognisable

std::error_code last_error() { return {errno, std::generic_category()}; }

// A file descriptor closed on every path.
class Fd {
public:
    explicit Fd(int fd = -1) : fd_{fd} {}

    ~Fd() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&&) = delete;
    Fd& operator=(Fd&&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }

    // Closes now; the error of close() matters for written files.
    std::error_code close_now() {
        const int fd = std::exchange(fd_, -1);
        return fd >= 0 && close(fd) != 0 ? last_error() : std::error_code{};
    }

private:
    int fd_;
};

std::optional<struct stat> status_of(const std::filesystem::path& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) {
        return std::nullopt;
    }
    return info;
}

bool taken(const std::filesystem::path& path) { return status_of(path).has_value(); }

// The names in directory, unsorted; empty when it cannot be read.
std::vector<std::string> names_in(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    std::error_code ignored;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{directory, ignored}) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

// rename(2) that never replaces (with a check first where the file system lacks it).
std::error_code rename_new(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return last_error();
    }
    if (taken(to)) {
        return std::make_error_code(std::errc::file_exists);
    }
    return std::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : last_error();
}

std::error_code rename_over(const std::filesystem::path& from, const std::filesystem::path& to) {
    return std::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : last_error();
}

void sync_directory(const std::filesystem::path& directory) {
    const Fd fd{open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (fd.get() >= 0) {
        fsync(fd.get());
    }
}

// "P1.ORF" -> "P1 2.ORF"; a folder or a name without extension: "Trip 2".
std::string numbered(const std::string& name, bool folder, int number) {
    const std::size_t dot = name.rfind('.');
    if (folder || dot == std::string::npos || dot == 0) {
        return std::format("{} {}", name, number);
    }
    return std::format("{} {}{}", name.substr(0, dot), number, name.substr(dot));
}

// "3.4 GB", as the status bar writes sizes (decimal units).
std::string human_size(std::uint64_t bytes) {
    constexpr std::array<std::string_view, 5> units{"bytes", "kB", "MB", "GB", "TB"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < units.size()) {
        value /= 1000.0;
        ++unit;
    }
    return unit == 0 ? i18n::trf("{} bytes", bytes)
                     : i18n::decimal(std::format("{:.1f} {}", value, units.at(unit)));
}

// One top-level item and the sidecars that travel with it.
struct Group {
    std::string name;
    struct stat info{};
    std::vector<std::string> sidecars;
};

class Engine {
public:
    Engine(const TransferRequest& request, const core::CancelToken& token,
           const TransferCallbacks& callbacks)
        : request_{request}, token_{token}, callbacks_{callbacks} {}

    TransferResult run();

private:
    [[nodiscard]] bool cancelled() {
        if (token_.cancelled()) {
            result_.cancelled = true;
        }
        return result_.cancelled;
    }

    void report() {
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
    }

    void error(const std::string& where, const std::error_code& code) {
        result_.errors.push_back(std::format("{}: {}", where, code.message()));
    }

    void error(const std::string& where, std::string_view what) {
        result_.errors.push_back(std::format("{}: {}", where, what));
    }

    void count(const std::filesystem::path& path);
    // Moves or copies one item to target; where names it in messages. false when it failed or
    // was cancelled (the source then stays).
    bool place(const std::filesystem::path& source, const std::filesystem::path& target,
               const std::string& where, bool replace);
    bool copy_file(const std::filesystem::path& source, const struct stat& info,
                   const std::filesystem::path& target, const std::string& where, bool replace);
    bool copy_link(const std::filesystem::path& source, const struct stat& info,
                   const std::filesystem::path& target, const std::string& where, bool replace);
    bool copy_folder(const std::filesystem::path& source, const struct stat& info,
                     const std::filesystem::path& target, const std::string& where);
    bool merge(const std::filesystem::path& source, const std::filesystem::path& target,
               const std::string& where);
    bool copy_data(int from, int to, const std::string& where);
    // A free hidden name next to target for a temporary.
    std::filesystem::path temporary(const std::filesystem::path& target);

    const TransferRequest& request_;
    const core::CancelToken& token_;
    const TransferCallbacks& callbacks_;
    TransferResult result_;
    TransferProgress progress_;
    bool copying_{false};  // data is copied (a copy, or a move between file systems)
    bool moving_{false};
    unsigned temporaries_{0};
};

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
void Engine::count(const std::filesystem::path& path) {
    const std::optional<struct stat> info = status_of(path);
    if (!info) {
        return;
    }
    if (S_ISDIR(info->st_mode)) {
        std::error_code ignored;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator{path, ignored}) {
            if (token_.cancelled()) {
                return;
            }
            count(entry.path());
        }
        return;
    }
    ++progress_.files_total;
    if (S_ISREG(info->st_mode)) {
        progress_.bytes_total += static_cast<std::uint64_t>(info->st_size);
    }
}

std::filesystem::path Engine::temporary(const std::filesystem::path& target) {
    const std::filesystem::path parent = target.parent_path();
    for (;;) {
        std::filesystem::path candidate =
            parent / std::format("{}{}-{}", kTemporary, getpid(), ++temporaries_);
        if (!taken(candidate)) {
            return candidate;
        }
    }
}

bool Engine::copy_data(int from, int to, const std::string& where) {
    bool fallback = false;
    std::vector<char> buffer;
    for (;;) {
        if (cancelled()) {
            return false;
        }
        ssize_t copied = 0;
        if (!fallback) {
            copied = copy_file_range(from, nullptr, to, nullptr, kChunk, 0);
            if (copied < 0 &&
                (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)) {
                fallback = true;  // not across these file systems: plain reads and writes
                continue;
            }
        } else {
            buffer.resize(kBuffer);
            copied = read(from, buffer.data(), buffer.size());
            for (ssize_t written = 0; copied > 0 && written < copied;) {
                const ssize_t now =
                    write(to, buffer.data() + written, static_cast<std::size_t>(copied - written));
                if (now < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    error(where, last_error());
                    return false;
                }
                written += now;
            }
        }
        if (copied < 0) {
            if (errno == EINTR) {
                continue;
            }
            error(where, last_error());
            return false;
        }
        if (copied == 0) {
            return true;
        }
        progress_.bytes_done += static_cast<std::uint64_t>(copied);
        result_.bytes_done += static_cast<std::uint64_t>(copied);
        report();
    }
}

bool Engine::copy_file(const std::filesystem::path& source, const struct stat& info,
                       const std::filesystem::path& target, const std::string& where,
                       bool replace) {
    const Fd from{open(source.c_str(), O_RDONLY | O_CLOEXEC)};
    if (from.get() < 0) {
        error(where, last_error());
        return false;
    }
    const std::filesystem::path part = temporary(target);
    Fd to{open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (to.get() < 0) {
        error(where, last_error());
        return false;
    }
    const auto give_up = [&part] { unlink(part.c_str()); };
    if (!copy_data(from.get(), to.get(), where)) {
        give_up();
        return false;
    }
    const std::array<timespec, 2> times{info.st_atim, info.st_mtim};
    fchmod(to.get(), info.st_mode & 07777);
    futimens(to.get(), times.data());
    // Between file systems the original is deleted next: the copy must be on the disk first.
    if (moving_ && fsync(to.get()) != 0) {
        error(where, last_error());
        give_up();
        return false;
    }
    if (const std::error_code closed = to.close_now()) {
        error(where, closed);
        give_up();
        return false;
    }
    if (const std::error_code placed =
            replace ? rename_over(part, target) : rename_new(part, target)) {
        error(where, placed);
        give_up();
        return false;
    }
    ++progress_.files_done;
    ++result_.files_done;
    if (moving_) {
        sync_directory(target.parent_path());
        if (unlink(source.c_str()) != 0) {
            error(where, last_error());
        }
    }
    report();
    return true;
}

bool Engine::copy_link(const std::filesystem::path& source, const struct stat& info,
                       const std::filesystem::path& target, const std::string& where,
                       bool replace) {
    std::string link(static_cast<std::size_t>(std::max<off_t>(info.st_size, 0)) + 1, '\0');
    const ssize_t length = readlink(source.c_str(), link.data(), link.size());
    if (length < 0) {
        error(where, last_error());
        return false;
    }
    link.resize(static_cast<std::size_t>(length));
    const std::filesystem::path part = temporary(target);
    if (symlink(link.c_str(), part.c_str()) != 0) {
        error(where, last_error());
        return false;
    }
    const std::array<timespec, 2> times{info.st_atim, info.st_mtim};
    utimensat(AT_FDCWD, part.c_str(), times.data(), AT_SYMLINK_NOFOLLOW);
    if (const std::error_code placed =
            replace ? rename_over(part, target) : rename_new(part, target)) {
        error(where, placed);
        unlink(part.c_str());
        return false;
    }
    ++progress_.files_done;
    ++result_.files_done;
    if (moving_ && unlink(source.c_str()) != 0) {
        error(where, last_error());
    }
    report();
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
bool Engine::copy_folder(const std::filesystem::path& source, const struct stat& info,
                         const std::filesystem::path& target, const std::string& where) {
    // Built under a hidden name and renamed when complete: a cancelled copy leaves no half folder.
    const std::filesystem::path part = temporary(target);
    if (mkdir(part.c_str(), 0700) != 0) {
        error(where, last_error());
        return false;
    }
    const auto give_up = [&part] {
        std::error_code ignored;
        std::filesystem::remove_all(part, ignored);
    };
    bool complete = true;
    std::error_code listed;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{source, listed}) {
        const std::string name = entry.path().filename().string();
        if (!place(entry.path(), part / name, std::format("{}/{}", where, name), false)) {
            complete = false;
            if (result_.cancelled) {
                give_up();
                return false;
            }
        }
    }
    if (listed) {
        error(where, listed);
        complete = false;
    }
    chmod(part.c_str(), info.st_mode & 07777);
    const std::array<timespec, 2> times{info.st_atim, info.st_mtim};
    utimensat(AT_FDCWD, part.c_str(), times.data(), 0);
    if (const std::error_code placed = rename_new(part, target)) {
        error(where, placed);
        give_up();
        return false;
    }
    if (moving_ && complete) {
        sync_directory(target.parent_path());
        if (rmdir(source.c_str()) != 0) {
            error(where, last_error());
        }
    }
    return complete;
}

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
bool Engine::merge(const std::filesystem::path& source, const std::filesystem::path& target,
                   const std::string& where) {
    // Into a folder that is there: each entry in turn, the same names replaced.
    bool complete = true;
    std::error_code listed;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{source, listed}) {
        const std::string name = entry.path().filename().string();
        if (!place(entry.path(), target / name, std::format("{}/{}", where, name), true)) {
            complete = false;
            if (result_.cancelled) {
                return false;
            }
        }
    }
    if (listed) {
        error(where, listed);
        complete = false;
    }
    if (moving_ && complete && rmdir(source.c_str()) != 0) {
        error(where, last_error());
    }
    return complete;
}

// NOLINTNEXTLINE(misc-no-recursion): a folder's depth; a link is copied, never followed
bool Engine::place(const std::filesystem::path& source, const std::filesystem::path& target,
                   const std::string& where, bool replace) {
    if (cancelled()) {
        return false;
    }
    const std::optional<struct stat> info = status_of(source);
    if (!info) {
        error(where, last_error());
        return false;
    }
    const std::optional<struct stat> existing = replace ? status_of(target) : std::nullopt;
    const bool folder = S_ISDIR(info->st_mode);
    if (existing && folder != S_ISDIR(existing->st_mode)) {
        error(where, std::string{folder ? i18n::tr("a file has its name; a folder does not replace a file")
                                        : i18n::tr("a folder has its name; a file does not replace a folder")});
        return false;
    }
    if (existing && folder) {
        return merge(source, target, where);
    }
    if (!copying_) {
        // A move on one file system: a rename, at once.
        const std::error_code moved =
            replace ? rename_over(source, target) : rename_new(source, target);
        if (!moved) {
            ++progress_.files_done;
            ++result_.files_done;
            report();
            return true;
        }
        if (moved != std::errc::cross_device_link) {
            error(where, moved);
            return false;
        }
        copying_ = true;  // a mount point below the source: copy from here on
    }
    if (folder) {
        return copy_folder(source, *info, target, where);
    }
    if (S_ISREG(info->st_mode)) {
        return copy_file(source, *info, target, where, replace);
    }
    if (S_ISLNK(info->st_mode)) {
        return copy_link(source, *info, target, where, replace);
    }
    error(where, std::string{i18n::tr("a special file (device, socket, pipe): not copied")});
    return false;
}

TransferResult Engine::run() {
    const TransferRequest& r = request_;
    moving_ = r.kind == TransferKind::move;
    const std::optional<struct stat> from = status_of(r.source);
    const std::optional<struct stat> to = status_of(r.destination);
    if (!to || !S_ISDIR(to->st_mode)) {
        result_.refused = i18n::trf("{} is not a folder", r.destination.string());
        return result_;
    }
    std::error_code ignored;
    const std::filesystem::path source_dir = std::filesystem::weakly_canonical(r.source, ignored);
    const std::filesystem::path target_dir =
        std::filesystem::weakly_canonical(r.destination, ignored);
    const bool same_folder = source_dir == target_dir;
    if (moving_ && same_folder) {
        result_.placed = r.names;  // already there
        return result_;
    }
    copying_ = !moving_ || r.force_copy || !from || from->st_dev != to->st_dev;

    // The items and their sidecars; a sidecar picked along with its photo goes with it.
    const std::vector<std::string> listing = names_in(r.source);
    std::vector<Group> groups;
    for (const std::string& name : r.names) {
        Group group{.name = name, .info = {}, .sidecars = {}};
        const std::optional<struct stat> info = status_of(r.source / name);
        if (!info) {
            error(name, last_error());
            continue;
        }
        group.info = *info;
        if (S_ISREG(info->st_mode)) {
            group.sidecars = sidecars_of(listing, name);
        }
        groups.push_back(std::move(group));
    }
    std::erase_if(groups, [&groups](const Group& item) {
        return std::ranges::any_of(groups, [&item](const Group& other) {
            return std::ranges::find(other.sidecars, item.name) != other.sidecars.end();
        });
    });

    for (const Group& group : groups) {
        if (!S_ISDIR(group.info.st_mode)) {
            continue;
        }
        const std::filesystem::path folder = source_dir / group.name;
        const auto [end, rest] = std::ranges::mismatch(folder, target_dir);
        if (end == folder.end()) {
            result_.refused =
                moving_ ? i18n::trf("Cannot move {} into itself", group.name)
                        : i18n::trf("Cannot copy {} into itself", group.name);
            return result_;
        }
    }

    progress_.preparing = true;
    report();
    for (const Group& group : groups) {
        if (copying_) {
            count(r.source / group.name);
            for (const std::string& sidecar : group.sidecars) {
                count(r.source / sidecar);
            }
        } else {
            progress_.files_total += 1 + group.sidecars.size();
        }
    }
    progress_.preparing = false;
    if (cancelled()) {
        return result_;
    }
    if (copying_) {
        struct statvfs space{};
        if (statvfs(r.destination.c_str(), &space) == 0) {
            const std::uint64_t free = static_cast<std::uint64_t>(space.f_bavail) *
                                       static_cast<std::uint64_t>(space.f_frsize);
            if (progress_.bytes_total > free) {
                result_.refused = i18n::trf("Not enough space: {} needed, {} free",
                                              human_size(progress_.bytes_total), human_size(free));
                return result_;
            }
        }
    }
    report();

    for (const Group& group : groups) {
        if (cancelled()) {
            break;
        }
        const bool folder = S_ISDIR(group.info.st_mode);
        std::string name = group.name;
        const auto sidecar_target = [&group](const std::string& sidecar, const std::string& as) {
            return sidecar_name(sidecar, group.name, as);
        };
        bool conflict = taken(r.destination / name);
        for (const std::string& sidecar : group.sidecars) {
            conflict = conflict || taken(r.destination / sidecar_target(sidecar, name));
        }
        bool replace = false;
        if (conflict) {
            Resolution resolution = Resolution::keep_both;  // a copy into its own folder
            if (!same_folder) {
                const std::optional<struct stat> there = status_of(r.destination / name);
                resolution =
                    callbacks_.resolve
                        ? callbacks_.resolve({.name = name,
                                              .destination = r.destination,
                                              .directory = folder,
                                              .target_directory = there && S_ISDIR(there->st_mode)})
                        : Resolution::skip;
            }
            if (resolution == Resolution::cancel) {
                result_.cancelled = true;
                break;
            }
            if (resolution == Resolution::skip) {
                continue;
            }
            if (resolution == Resolution::keep_both) {
                name = free_name(r.destination, group.name, folder, group.sidecars);
            } else {
                replace = true;
            }
        }
        progress_.current = name;
        report();
        if (!place(r.source / group.name, r.destination / name, group.name, replace)) {
            if (result_.cancelled) {
                break;
            }
            continue;  // the photo did not arrive: its sidecars stay with it
        }
        result_.placed.push_back(name);
        result_.items.push_back({.from = r.source / group.name, .to = r.destination / name});
        for (const std::string& sidecar : group.sidecars) {
            if (!place(r.source / sidecar, r.destination / sidecar_target(sidecar, name), sidecar,
                       replace) &&
                result_.cancelled) {
                break;
            }
        }
    }
    progress_.current.clear();
    report();
    return result_;
}

}  // namespace

std::string free_name(const std::filesystem::path& directory, const std::string& name, bool folder,
                      const std::vector<std::string>& sidecars) {
    for (int number = 2;; ++number) {
        std::string candidate = numbered(name, folder, number);
        bool free = !taken(directory / candidate);
        for (const std::string& sidecar : sidecars) {
            free = free && !taken(directory / sidecar_name(sidecar, name, candidate));
        }
        if (free) {
            return candidate;
        }
    }
}

TransferResult run_transfer(const TransferRequest& request, const core::CancelToken& token,
                            const TransferCallbacks& callbacks) {
    return Engine{request, token, callbacks}.run();
}

}  // namespace nordend::fs
