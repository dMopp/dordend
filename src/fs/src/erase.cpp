#include "rummage/fs/erase.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "rummage/fs/operations.hpp"
#include "rummage/fs/trash.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::fs {
namespace {

std::string error_text(int code) {
    return std::error_code{code, std::generic_category()}.message();
}

// One item to delete and what goes with it.
struct Group {
    std::filesystem::path item;
    std::string name;  // as the user knows it
    std::vector<std::filesystem::path> sidecars;
    std::filesystem::path trash;  // the trash it is in; empty outside the trash
};

// The names of a folder's entries (".", ".." left out); fd stays open.
std::vector<std::string> names_in(int fd) {
    std::vector<std::string> names;
    const int copy = dup(fd);  // closedir closes the fd it is given
    if (copy < 0) {
        return names;
    }
    DIR* const dir = fdopendir(copy);
    if (dir == nullptr) {
        close(copy);
        return names;
    }
    rewinddir(dir);
    while (const dirent* const entry = readdir(dir)) {
        const std::string_view name{entry->d_name};
        if (name != "." && name != "..") {
            names.emplace_back(name);
        }
    }
    closedir(dir);
    return names;
}

std::vector<std::string> names_in(const std::filesystem::path& folder) {
    const int fd = open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    std::vector<std::string> names = names_in(fd);
    close(fd);
    return names;
}

// The items of request (ErasePlan's comment), grouped with their sidecars.
std::vector<Group> groups_of(const TransferRequest& request, std::vector<std::string>& errors) {
    std::vector<Group> groups;
    if (request.kind == TransferKind::empty_trash) {
        for (const std::string& trash : request.names) {
            const std::filesystem::path files = std::filesystem::path{trash} / "files";
            for (std::string& name : names_in(files)) {
                groups.push_back({.item = files / name,
                                  .name = std::move(name),
                                  .sidecars = {},
                                  .trash = trash});
            }
        }
        return groups;
    }
    if (is_trash(request.source)) {
        std::vector<std::filesystem::path> paths{request.names.begin(), request.names.end()};
        std::vector<std::optional<TrashedGroup>> found = trashed_groups(paths);
        for (std::size_t i = 0; i < paths.size(); ++i) {
            std::optional<TrashedGroup>& group = found[i];
            if (!group) {
                errors.push_back(
                    i18n::trf("{}: not in the trash any more", paths[i].filename().string()));
                continue;
            }
            groups.push_back({.item = std::move(group->item),
                              .name = group->info.original.filename().string(),
                              .sidecars = std::move(group->sidecars),
                              .trash = paths[i].parent_path().parent_path()});
        }
    } else {
        const std::vector<std::string> listing = names_in(request.source);
        for (const std::string& name : request.names) {
            const std::filesystem::path item = request.source / name;
            struct stat info{};
            if (lstat(item.c_str(), &info) != 0) {
                errors.push_back(std::format("{}: {}", name, error_text(errno)));
                continue;
            }
            Group group{.item = item, .name = name, .sidecars = {}, .trash = {}};
            if (S_ISREG(info.st_mode)) {
                for (const std::string& sidecar : sidecars_of(listing, name)) {
                    group.sidecars.push_back(request.source / sidecar);
                }
            }
            groups.push_back(std::move(group));
        }
    }
    // A sidecar picked along with its photo goes with it.
    std::erase_if(groups, [&groups](const Group& group) {
        return std::ranges::any_of(groups, [&group](const Group& other) {
            return std::ranges::find(other.sidecars, group.item) != other.sidecars.end();
        });
    });
    return groups;
}

// Adds up what a delete removes.
class Counter {
public:
    explicit Counter(const core::CancelToken& token) : token_{token} {}

    // NOLINTNEXTLINE(misc-no-recursion): as deep as the tree
    void count(int parent, const char* name) {
        if (token_.cancelled()) {
            return;
        }
        struct stat info{};
        if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
            return;
        }
        ++entries_;
        if (S_ISREG(info.st_mode) &&
            (info.st_nlink <= 1 || linked_.emplace(info.st_dev, info.st_ino).second)) {
            bytes_ += static_cast<std::uint64_t>(info.st_size);
        }
        if (!S_ISDIR(info.st_mode)) {
            return;
        }
        const int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            return;  // what cannot be read is not counted
        }
        for (const std::string& child : names_in(fd)) {
            count(fd, child.c_str());
        }
        close(fd);
    }

    [[nodiscard]] std::size_t entries() const noexcept { return entries_; }

    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }

private:
    const core::CancelToken& token_;
    std::set<std::pair<dev_t, ino_t>> linked_;
    std::size_t entries_{0};
    std::uint64_t bytes_{0};
};

class Eraser {
public:
    Eraser(const TransferRequest& request, const core::CancelToken& token,
           const TransferCallbacks& callbacks)
        : request_{request}, token_{token}, callbacks_{callbacks} {}

    TransferResult run();

private:
    void report() {
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
    }

    // Removes name in the folder parent, what it holds first; false when anything stays. shown:
    // the name in messages ("Trip/P1.ORF").
    bool remove(int parent, const std::string& name, const std::string& shown);

    bool remove(const std::filesystem::path& path, const std::string& shown) {
        return remove(AT_FDCWD, path.string(), shown);
    }

    // Removes what the folder name in parent holds.
    bool empty_folder(int parent, const std::string& name, const struct stat& info,
                      const std::string& shown);

    // An item gone from its trash: its .trashinfo goes too.
    void forget(const Group& group, const std::filesystem::path& item, bool folder);

    const TransferRequest& request_;
    const core::CancelToken& token_;
    const TransferCallbacks& callbacks_;
    std::set<std::filesystem::path> sized_;  // trashes that lost a folder: directorysizes
    TransferResult result_;
    TransferProgress progress_;
};

// NOLINTNEXTLINE(misc-no-recursion): as deep as the tree
bool Eraser::remove(int parent, const std::string& name, const std::string& shown) {
    if (token_.cancelled()) {
        result_.cancelled = true;
        return false;
    }
    struct stat info{};
    if (fstatat(parent, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return true;  // gone already
        }
        result_.errors.push_back(std::format("{}: {}", shown, error_text(errno)));
        return false;
    }
    if (S_ISDIR(info.st_mode)) {
        if (!empty_folder(parent, name, info, shown)) {
            return false;
        }
        if (unlinkat(parent, name.c_str(), AT_REMOVEDIR) != 0) {
            result_.errors.push_back(std::format("{}: {}", shown, error_text(errno)));
            return false;
        }
    } else if (unlinkat(parent, name.c_str(), 0) != 0) {
        result_.errors.push_back(std::format("{}: {}", shown, error_text(errno)));
        return false;
    }
    if (S_ISREG(info.st_mode) && info.st_nlink <= 1) {
        result_.bytes_done += static_cast<std::uint64_t>(info.st_size);  // its last link
    }
    ++result_.files_done;
    ++progress_.files_done;
    progress_.bytes_done = result_.bytes_done;
    report();
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): as deep as the tree
bool Eraser::empty_folder(int parent, const std::string& name, const struct stat& info,
                          const std::string& shown) {
    constexpr mode_t open_up = S_IRWXU;
    if ((info.st_mode & open_up) != open_up && info.st_uid == geteuid()) {
        // Read-only folders (a Go module cache): opened up, they are going anyway. A failure shows
        // as the open's or the removal's error below.
        [[maybe_unused]] const int opened_up =
            fchmodat(parent, name.c_str(), (info.st_mode & 07777U) | open_up, 0);
    }
    const int fd = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        result_.errors.push_back(std::format("{}: {}", shown, error_text(errno)));
        return false;
    }
    bool all = true;
    for (const std::string& child : names_in(fd)) {
        if (!remove(fd, child, std::format("{}/{}", shown, child))) {
            all = false;
            if (result_.cancelled) {
                break;
            }
        }
    }
    close(fd);
    return all;
}

void Eraser::forget(const Group& group, const std::filesystem::path& item, bool folder) {
    if (group.trash.empty()) {
        return;
    }
    unlink(info_file_of(item).c_str());
    if (folder) {
        sized_.insert(group.trash);
    }
}

TransferResult Eraser::run() {
    const std::vector<Group> groups = groups_of(request_, result_.errors);
    progress_.preparing = true;
    report();
    Counter counter{token_};
    for (const Group& group : groups) {
        counter.count(AT_FDCWD, group.item.c_str());
        for (const std::filesystem::path& sidecar : group.sidecars) {
            counter.count(AT_FDCWD, sidecar.c_str());
        }
    }
    progress_.preparing = false;
    progress_.files_total = counter.entries();
    progress_.bytes_total = counter.bytes();
    report();

    for (const Group& group : groups) {
        if (token_.cancelled()) {
            result_.cancelled = true;
            break;
        }
        progress_.current = group.name;
        struct stat info{};
        const bool folder = lstat(group.item.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
        if (!remove(group.item, group.name)) {
            if (folder) {
                sized_.insert(group.trash);  // what it held is less now
            }
            continue;  // the photo stays: its sidecars stay with it
        }
        forget(group, group.item, folder);
        result_.placed.push_back(group.name);
        for (const std::filesystem::path& sidecar : group.sidecars) {
            if (remove(sidecar, sidecar.filename().string())) {
                forget(group, sidecar, false);
            }
        }
    }

    if (request_.kind == TransferKind::empty_trash) {
        // Every .trashinfo whose item is gone, the orphans too.
        for (const std::string& name : request_.names) {
            const std::filesystem::path trash{name};
            for (const std::string& file : names_in(trash / "info")) {
                const std::filesystem::path info = trash / "info" / file;
                constexpr std::string_view suffix = ".trashinfo";
                if (!file.ends_with(suffix)) {
                    continue;
                }
                const std::string_view item =
                    std::string_view{file}.substr(0, file.size() - suffix.size());
                struct stat there{};
                if (lstat((trash / "files" / item).c_str(), &there) != 0 && errno == ENOENT) {
                    unlink(info.c_str());
                }
            }
            sized_.insert(trash);
        }
    }
    for (const std::filesystem::path& trash : sized_) {
        if (!trash.empty()) {
            prune_directory_sizes(trash);
        }
    }
    progress_.current.clear();
    report();
    return result_;
}

}  // namespace

ErasePlan plan_erase(const TransferRequest& request, const core::CancelToken& token) {
    ErasePlan plan;
    Counter counter{token};
    for (Group& group : groups_of(request, plan.errors)) {
        counter.count(AT_FDCWD, group.item.c_str());
        plan.items.push_back(std::move(group.item));
        plan.names.push_back(std::move(group.name));
        for (std::filesystem::path& sidecar : group.sidecars) {
            counter.count(AT_FDCWD, sidecar.c_str());
            // One picked along with its photo was counted as an item by whoever picked it.
            const std::string picked =
                is_trash(request.source) ? sidecar.string() : sidecar.filename().string();
            if (std::ranges::find(request.names, picked) == request.names.end()) {
                ++plan.sidecars;
            }
            plan.items.push_back(std::move(sidecar));
        }
    }
    plan.entries = counter.entries();
    plan.bytes = counter.bytes();
    return plan;
}

TransferResult run_erase(const TransferRequest& request, const core::CancelToken& token,
                         const TransferCallbacks& callbacks) {
    return Eraser{request, token, callbacks}.run();
}

}  // namespace rummage::fs
