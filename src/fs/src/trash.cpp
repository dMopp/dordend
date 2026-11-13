#include "rummage/fs/trash.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

#include "rummage/core/file.hpp"
#include "rummage/fs/operations.hpp"
#include "rummage/fs/places.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::fs {
namespace {

constexpr std::string_view kInfoSuffix = ".trashinfo";
constexpr std::string_view kSizes = "directorysizes";

std::error_code last_error() { return {errno, std::generic_category()}; }

std::optional<struct stat> status_of(const std::filesystem::path& path) {
    struct stat info{};
    if (lstat(path.c_str(), &info) != 0) {
        return std::nullopt;
    }
    return info;
}

// mkdir that is fine with a folder already there.
std::error_code make_directory(const std::filesystem::path& path) {
    if (mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) {
        return {};
    }
    return last_error();
}

// mkdir -p, the folders made private (the home trash and, when missing, its parents).
// NOLINTNEXTLINE(misc-no-recursion): as deep as the path
std::error_code make_directories(const std::filesystem::path& path) {
    if (status_of(path)) {
        return {};
    }
    if (path.has_parent_path() && path.parent_path() != path) {
        if (const std::error_code error = make_directories(path.parent_path())) {
            return error;
        }
    }
    return make_directory(path);
}

std::error_code rename_new(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return last_error();
    }
    if (status_of(to)) {
        return std::make_error_code(std::errc::file_exists);
    }
    return std::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : last_error();
}

// Writes all of text to fd.
std::error_code write_all(int fd, std::string_view text) {
    while (!text.empty()) {
        const ssize_t written = write(fd, text.data(), text.size());
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return last_error();
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
    return {};
}

std::string unescape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = hex(text[i + 1]);
            const int low = hex(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4U) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

// What du -B1 says: the blocks of path and everything in it, a hard link counted once.
std::uint64_t disk_usage(const std::filesystem::path& path) {
    std::uint64_t bytes = 0;
    std::set<std::pair<dev_t, ino_t>> linked;
    const auto add = [&](const struct stat& info) {
        if (info.st_nlink > 1 && !S_ISDIR(info.st_mode) &&
            !linked.emplace(info.st_dev, info.st_ino).second) {
            return;
        }
        bytes += static_cast<std::uint64_t>(info.st_blocks) * 512U;
    };
    if (const std::optional<struct stat> info = status_of(path)) {
        add(*info);
    }
    std::error_code ignored;
    for (std::filesystem::recursive_directory_iterator it{path, ignored}, end; it != end;
         it.increment(ignored)) {
        if (const std::optional<struct stat> info = status_of(it->path())) {
            add(*info);
        }
    }
    return bytes;
}

// A trashed folder, for directorysizes.
struct SizedFolder {
    std::string name;  // in files/
    std::uint64_t bytes{0};
    std::int64_t info_mtime{0};
};

// Adds folders to trash/directorysizes: through a temporary renamed over it, as the
// specification asks; lines of folders no longer in files/ are dropped.
void add_directory_sizes(const std::filesystem::path& trash,
                         const std::vector<SizedFolder>& folders) {
    std::string text;
    {
        std::ifstream in{trash / kSizes};
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t last = line.rfind(' ');
            if (last == std::string::npos) {
                continue;
            }
            const std::string name = unescape(std::string_view{line}.substr(last + 1));
            if (name.empty() || name.find('/') != std::string::npos ||
                !status_of(trash / "files" / name) ||
                std::ranges::any_of(folders,
                                    [&name](const SizedFolder& f) { return f.name == name; })) {
                continue;
            }
            text += line;
            text += '\n';
        }
    }
    for (const SizedFolder& folder : folders) {
        text += std::format("{} {} {}\n", folder.bytes, folder.info_mtime,
                            escape_path(folder.name, false));
    }
    std::string pattern = (trash / std::format("{}.XXXXXX", kSizes)).string();
    const int fd = mkostemp(pattern.data(), O_CLOEXEC);
    if (fd < 0) {
        return;  // a cache: it can be made again later
    }
    const bool written = !write_all(fd, text) && fchmod(fd, 0600) == 0;
    if (close(fd) != 0 || !written || std::rename(pattern.c_str(), (trash / kSizes).c_str()) != 0) {
        unlink(pattern.c_str());
    }
}

// name with bytes dropped from its front (at a UTF-8 character's start) so that extra more fit
// in NAME_MAX; empty when nothing would be left.
std::string shorten_front(const std::string& name, std::size_t extra) {
    if (extra >= NAME_MAX) {
        return {};
    }
    std::size_t cut = name.size() > NAME_MAX - extra ? name.size() - (NAME_MAX - extra) : 0;
    while (cut < name.size() && (static_cast<unsigned char>(name[cut]) & 0xC0U) == 0x80U) {
        ++cut;
    }
    return name.substr(cut);
}

class Trasher {
public:
    Trasher(const TransferRequest& request, const core::CancelToken& token,
            const TransferCallbacks& callbacks)
        : request_{request}, token_{token}, callbacks_{callbacks} {}

    TransferResult run();

private:
    void report() {
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
    }

    void error(const std::string& where, const std::error_code& code) {
        result_.errors.push_back(std::format("{}: {}", where, code.message()));
    }

    // Trashes source/name: where it went in files/; nullopt when it stays.
    std::optional<std::filesystem::path> trash_item(const std::string& name);

    // The trash for the items of source, made if needed; nullopt and refused set when none.
    std::optional<TrashDir> find_trash();

    const TransferRequest& request_;
    const core::CancelToken& token_;
    const TransferCallbacks& callbacks_;
    std::filesystem::path source_;  // canonical
    TrashDir trash_;
    std::vector<SizedFolder> folders_;
    TransferResult result_;
    TransferProgress progress_;
};

std::optional<TrashDir> Trasher::find_trash() {
    struct stat folder{};
    if (stat(source_.c_str(), &folder) != 0) {
        result_.refused = std::format("{}: {}", source_.string(), last_error().message());
        return std::nullopt;
    }
    const std::filesystem::path& home = request_.destination;
    if (!home.empty() && request_.trash_top.empty()) {
        if (const std::error_code made = make_directories(home)) {
            result_.refused =
                i18n::trf("Cannot make the trash {}: {}", home.string(), made.message());
            return std::nullopt;
        }
        struct stat there{};
        if (stat(home.c_str(), &there) == 0 && there.st_dev == folder.st_dev) {
            if (const std::error_code made = make_directory(home / "files");
                made || make_directory(home / "info")) {
                result_.refused = i18n::trf("Cannot make the trash {}: {}", home.string(),
                                              (made ? made : last_error()).message());
                return std::nullopt;
            }
            return TrashDir{.path = home, .relative_to = {}};
        }
    }
    const std::filesystem::path top =
        request_.trash_top.empty() ? mount_top(source_) : request_.trash_top;
    std::error_code why;
    std::optional<TrashDir> trash = top_trash(top, geteuid(), why);
    if (!trash) {
        result_.refused = i18n::trf("No trash on the disk of {}: {} cannot hold one ({})",
                                      source_.filename().string(), top.string(), why.message());
    }
    return trash;
}

std::optional<std::filesystem::path> Trasher::trash_item(const std::string& name) {
    const std::filesystem::path item = source_ / name;
    const std::optional<struct stat> info = status_of(item);
    if (!info) {
        error(name, last_error());
        return std::nullopt;
    }
    std::filesystem::path original = item;
    if (!trash_.relative_to.empty()) {
        // Relative to the top directory, as the specification prefers there.
        original = item.lexically_relative(trash_.relative_to);
        if (original.empty() || *original.begin() == "..") {
            original = item;
        }
    }
    const std::string contents = trash_info(original, std::time(nullptr));

    std::string base = name;
    for (int id = 1;; ++id) {
        std::string candidate = trash_name(base, id);
        if (candidate.size() + kInfoSuffix.size() > NAME_MAX) {
            base = shorten_front(base, candidate.size() - base.size() + kInfoSuffix.size());
            if (base.empty()) {
                error(name, std::make_error_code(std::errc::filename_too_long));
                return std::nullopt;
            }
            id = 0;
            continue;
        }
        const std::filesystem::path info_file =
            trash_.path / "info" / (candidate + std::string{kInfoSuffix});
        const int fd = open(info_file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            error(name, last_error());
            return std::nullopt;
        }
        // Complete before the item moves: nobody reads an empty one.
        const std::error_code written = write_all(fd, contents);
        if (close(fd) != 0 || written) {
            error(name, written ? written : last_error());
            unlink(info_file.c_str());
            return std::nullopt;
        }
        const std::error_code moved = rename_new(item, trash_.path / "files" / candidate);
        if (moved == std::errc::file_exists) {
            unlink(info_file.c_str());  // files/ holds one without its info: leave it be
            continue;
        }
        if (moved) {
            unlink(info_file.c_str());
            if (moved == std::errc::cross_device_link) {
                result_.errors.push_back(
                    i18n::trf("{}: on another disk than its trash (a mount point?)", name));
            } else {
                error(name, moved);
            }
            return std::nullopt;
        }
        if (S_ISDIR(info->st_mode)) {
            struct stat written_info{};
            stat(info_file.c_str(), &written_info);
            folders_.push_back({.name = candidate,
                                .bytes = disk_usage(trash_.path / "files" / candidate),
                                .info_mtime = static_cast<std::int64_t>(written_info.st_mtime)});
        }
        ++progress_.files_done;
        ++result_.files_done;
        report();
        return trash_.path / "files" / candidate;
    }
}

TransferResult Trasher::run() {
    std::error_code canonical;
    source_ = std::filesystem::canonical(request_.source, canonical);
    if (canonical) {
        result_.refused = std::format("{}: {}", request_.source.string(), canonical.message());
        return result_;
    }
    std::optional<TrashDir> trash = find_trash();
    if (!trash) {
        return result_;
    }
    trash_ = std::move(*trash);
    const std::filesystem::path trash_path =
        std::filesystem::weakly_canonical(trash_.path, canonical);
    const auto within = [](const std::filesystem::path& inner, const std::filesystem::path& outer) {
        const auto [end, rest] = std::ranges::mismatch(outer, inner);
        return end == outer.end();
    };
    if (within(source_, trash_path)) {
        result_.refused = i18n::tr("These are in the trash already");
        return result_;
    }

    // The items and their sidecars; a sidecar picked along with its photo goes with it.
    std::vector<std::string> listing;
    {
        std::error_code ignored;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator{source_, ignored}) {
            listing.push_back(entry.path().filename().string());
        }
    }

    struct Group {
        std::string name;
        std::vector<std::string> sidecars;
    };

    std::vector<Group> groups;
    for (const std::string& name : request_.names) {
        const std::optional<struct stat> info = status_of(source_ / name);
        if (!info) {
            error(name, last_error());
            continue;
        }
        if (within(trash_path, source_ / name)) {
            result_.errors.push_back(i18n::trf("{}: it holds the trash", name));
            continue;
        }
        groups.push_back({.name = name,
                          .sidecars = S_ISREG(info->st_mode) ? sidecars_of(listing, name)
                                                             : std::vector<std::string>{}});
    }
    std::erase_if(groups, [&groups](const Group& item) {
        return std::ranges::any_of(groups, [&item](const Group& other) {
            return std::ranges::find(other.sidecars, item.name) != other.sidecars.end();
        });
    });
    for (const Group& group : groups) {
        progress_.files_total += 1 + group.sidecars.size();
    }
    report();

    for (const Group& group : groups) {
        if (token_.cancelled()) {
            result_.cancelled = true;
            break;
        }
        progress_.current = group.name;
        const std::optional<std::filesystem::path> trashed = trash_item(group.name);
        if (!trashed) {
            continue;  // the photo stays: its sidecars stay with it
        }
        result_.placed.push_back(group.name);
        result_.items.push_back({.from = source_ / group.name, .to = *trashed});
        for (const std::string& sidecar : group.sidecars) {
            static_cast<void>(trash_item(sidecar));
        }
    }
    if (!folders_.empty()) {
        add_directory_sizes(trash_.path, folders_);
    }
    progress_.current.clear();
    report();
    return result_;
}

std::vector<std::filesystem::path> parse_mount_points(std::string_view text) {
    std::vector<std::filesystem::path> points;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        for (int field = 0; field < 4 && !line.empty(); ++field) {
            line.remove_prefix(std::min(line.find(' '), line.size()));
            line.remove_prefix(line.empty() ? 0 : 1);
        }
        const std::string_view escaped = line.substr(0, line.find(' '));
        std::string point;
        for (std::size_t i = 0; i < escaped.size(); ++i) {
            if (escaped[i] == '\\' && i + 3 < escaped.size()) {
                const int value = ((escaped[i + 1] - '0') * 64) + ((escaped[i + 2] - '0') * 8) +
                                  (escaped[i + 3] - '0');
                point.push_back(static_cast<char>(value));
                i += 3;
                continue;
            }
            point.push_back(escaped[i]);
        }
        if (point.starts_with('/')) {
            points.emplace_back(std::move(point));
        }
    }
    return points;
}

bool is_folder(const std::filesystem::path& path) {
    const std::optional<struct stat> info = status_of(path);
    return info && S_ISDIR(info->st_mode);
}

// A trash dir of the user's with its two folders.
bool usable_trash(const std::filesystem::path& dir, uid_t uid) {
    const std::optional<struct stat> info = status_of(dir);
    return info && S_ISDIR(info->st_mode) && info->st_uid == uid && is_folder(dir / "files") &&
           is_folder(dir / "info");
}

// What relative paths in a trash's .trashinfo files start from: the top directory, or for the
// home trash the folder it is in ($XDG_DATA_HOME).
std::filesystem::path relative_base(const std::filesystem::path& trash) {
    const std::string name = trash.filename().string();
    if (name.starts_with(".Trash-")) {
        return trash.parent_path();
    }
    if (trash.parent_path().filename() == ".Trash") {
        return trash.parent_path().parent_path();
    }
    return trash.parent_path();
}

// mkdir -p with mode for the folders made (a restored item's folder: the usual 0777 less the
// umask, not the trash's private 0700).
// NOLINTNEXTLINE(misc-no-recursion): as deep as the path
std::error_code make_folders(const std::filesystem::path& path) {
    if (status_of(path)) {
        return {};
    }
    if (path.has_parent_path() && path.parent_path() != path) {
        if (const std::error_code error = make_folders(path.parent_path())) {
            return error;
        }
    }
    if (mkdir(path.c_str(), 0777) == 0 || errno == EEXIST) {
        return {};
    }
    return last_error();
}

// An item found in a trash for restoring.
struct Found {
    std::filesystem::path item;       // in files/
    std::filesystem::path info_file;  // in info/
    TrashInfo info;
    bool directory{false};
    bool regular{false};
};

// The items of one trash with their info, read once per trash.
std::vector<Found> read_trash(const std::filesystem::path& trash) {
    std::vector<Found> found;
    const std::filesystem::path base = relative_base(trash);
    std::error_code ignored;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{trash / "info", ignored}) {
        const std::string file = entry.path().filename().string();
        if (!file.ends_with(kInfoSuffix)) {
            continue;
        }
        const core::TextFile text = core::read_text_file(entry.path());
        std::optional<TrashInfo> info = parse_trash_info(text.text, base);
        const std::filesystem::path item =
            trash / "files" / file.substr(0, file.size() - kInfoSuffix.size());
        const std::optional<struct stat> status = status_of(item);
        if (!info || !status) {
            continue;
        }
        found.push_back({.item = item,
                         .info_file = entry.path(),
                         .info = std::move(*info),
                         .directory = S_ISDIR(status->st_mode),
                         .regular = S_ISREG(status->st_mode)});
    }
    return found;
}

// The darktable sidecars that go with item (a regular file) when it leaves the trash: trashed
// from the same folder; the newest of each name.
std::vector<Found> trashed_sidecars(const std::vector<Found>& trash, const Found& item) {
    std::vector<Found> sidecars;
    if (!item.regular) {
        return sidecars;
    }
    const std::filesystem::path folder = item.info.original.parent_path();
    std::vector<std::string> names;
    for (const Found& other : trash) {
        if (other.info.original.parent_path() == folder) {
            names.push_back(other.info.original.filename().string());
        }
    }
    for (const std::string& sidecar : sidecars_of(names, item.info.original.filename().string())) {
        const Found* newest = nullptr;
        for (const Found& other : trash) {
            if (other.info.original == folder / sidecar &&
                (newest == nullptr || other.info.deleted > newest->info.deleted)) {
                newest = &other;
            }
        }
        if (newest != nullptr) {
            sidecars.push_back(*newest);
        }
    }
    return sidecars;
}

class Restorer {
public:
    Restorer(const TransferRequest& request, const core::CancelToken& token,
             const TransferCallbacks& callbacks)
        : request_{request}, token_{token}, callbacks_{callbacks} {}

    TransferResult run();

private:
    void report() {
        if (callbacks_.progress) {
            callbacks_.progress(progress_);
        }
    }

    void error(const std::string& where, const std::error_code& code) {
        result_.errors.push_back(std::format("{}: {}", where, code.message()));
    }

    const std::vector<Found>& trash_of(const std::filesystem::path& trash) {
        auto it = trashes_.find(trash);
        if (it == trashes_.end()) {
            it = trashes_.emplace(trash, read_trash(trash)).first;
        }
        return it->second;
    }

    // Moves one item to directory/name; false when it stays.
    bool put_back(const Found& item, const std::filesystem::path& directory,
                  const std::string& name, bool replace);

    const TransferRequest& request_;
    const core::CancelToken& token_;
    const TransferCallbacks& callbacks_;
    std::map<std::filesystem::path, std::vector<Found>> trashes_;
    TransferResult result_;
    TransferProgress progress_;
};

bool Restorer::put_back(const Found& item, const std::filesystem::path& directory,
                        const std::string& name, bool replace) {
    const std::string where = item.info.original.filename().string();
    const std::filesystem::path target = directory / name;
    std::error_code moved;
    if (!replace) {
        moved = rename_new(item.item, target);
    } else if (std::rename(item.item.c_str(), target.c_str()) != 0) {
        moved = last_error();
    }
    if (moved == std::errc::cross_device_link) {
        result_.errors.push_back(
            i18n::trf("{}: its folder is on another disk now: not copied back", where));
        return false;
    }
    if (moved) {
        error(where, moved);
        return false;
    }
    unlink(item.info_file.c_str());
    ++progress_.files_done;
    ++result_.files_done;
    report();
    return true;
}

TransferResult Restorer::run() {
    struct Group {
        Found item;
        std::vector<Found> sidecars;
    };

    std::vector<Group> groups;
    for (const std::string& name : request_.names) {
        const std::filesystem::path item{name};
        const std::vector<Found>& trash = trash_of(item.parent_path().parent_path());
        const auto it = std::ranges::find(trash, item, &Found::item);
        if (it == trash.end()) {
            result_.errors.push_back(
                i18n::trf("{}: not in the trash any more", item.filename().string()));
            continue;
        }
        groups.push_back({.item = *it, .sidecars = trashed_sidecars(trash, *it)});
    }
    // A sidecar picked along with its photo goes with it.
    std::erase_if(groups, [&groups](const Group& group) {
        return std::ranges::any_of(groups, [&group](const Group& other) {
            return std::ranges::find(other.sidecars, group.item.item, &Found::item) !=
                   other.sidecars.end();
        });
    });
    for (const Group& group : groups) {
        progress_.files_total += 1 + group.sidecars.size();
    }
    report();

    for (const Group& group : groups) {
        if (token_.cancelled()) {
            result_.cancelled = true;
            break;
        }
        const std::filesystem::path directory = group.item.info.original.parent_path();
        const std::string original = group.item.info.original.filename().string();
        progress_.current = original;
        if (const std::error_code made = make_folders(directory)) {
            error(original, made);
            continue;
        }
        std::vector<std::string> sidecar_names;
        sidecar_names.reserve(group.sidecars.size());
        for (const Found& sidecar : group.sidecars) {
            sidecar_names.push_back(sidecar.info.original.filename().string());
        }
        std::string name = original;
        bool conflict = status_of(directory / name).has_value();
        for (const std::string& sidecar : sidecar_names) {
            conflict = conflict ||
                       status_of(directory / sidecar_name(sidecar, original, name)).has_value();
        }
        bool replace = false;
        if (conflict) {
            const std::optional<struct stat> there = status_of(directory / name);
            const bool target_directory = there && S_ISDIR(there->st_mode);
            const Resolution resolution =
                callbacks_.resolve ? callbacks_.resolve({.name = name,
                                                         .destination = directory,
                                                         .directory = group.item.directory,
                                                         .target_directory = target_directory,
                                                         .merge = false})
                                   : Resolution::skip;
            if (resolution == Resolution::cancel) {
                result_.cancelled = true;
                break;
            }
            if (resolution == Resolution::skip) {
                continue;
            }
            if (resolution == Resolution::keep_both) {
                name = free_name(directory, original, group.item.directory, sidecar_names);
            } else if (group.item.directory || target_directory) {
                result_.errors.push_back(
                    i18n::trf("{}: a folder is not replaced; keep both or skip", original));
                continue;
            } else {
                replace = true;
            }
        }
        if (!put_back(group.item, directory, name, replace)) {
            continue;  // the photo stays: its sidecars stay with it
        }
        result_.placed.push_back(name);
        result_.items.push_back({.from = group.item.item, .to = directory / name});
        for (const Found& sidecar : group.sidecars) {
            const std::string to =
                sidecar_name(sidecar.info.original.filename().string(), original, name);
            static_cast<void>(put_back(sidecar, directory, to, replace));
        }
    }
    progress_.current.clear();
    report();
    return result_;
}

}  // namespace

std::filesystem::path home_trash() {
    // NOLINTBEGIN(concurrency-mt-unsafe): read on the loop thread, before any setenv
    const char* const data = std::getenv("XDG_DATA_HOME");
    if (data != nullptr && *data == '/') {
        return std::filesystem::path{data} / "Trash";
    }
    const char* const home = std::getenv("HOME");
    // NOLINTEND(concurrency-mt-unsafe)
    if (home != nullptr && *home == '/') {
        return std::filesystem::path{home} / ".local/share/Trash";
    }
    return {};
}

std::filesystem::path mount_top(const std::filesystem::path& path) {
    std::error_code ignored;
    std::filesystem::path top = std::filesystem::canonical(path, ignored);
    if (top.empty()) {
        top = std::filesystem::absolute(path, ignored).lexically_normal();
    }
    struct stat here{};
    if (stat(top.c_str(), &here) != 0) {
        return top;
    }
    while (top.has_parent_path() && top.parent_path() != top) {
        struct stat above{};
        if (stat(top.parent_path().c_str(), &above) != 0 || above.st_dev != here.st_dev) {
            break;
        }
        top = top.parent_path();
    }
    return top;
}

std::optional<TrashDir> top_trash(const std::filesystem::path& topdir, uid_t uid,
                                  std::error_code& error) {
    // A folder of the user's, made when missing.
    const auto usable = [&error, uid](const std::filesystem::path& dir) {
        bool made = false;
        std::optional<struct stat> info = status_of(dir);
        if (!info) {
            if (mkdir(dir.c_str(), 0700) != 0) {
                error = last_error();
                return false;
            }
            made = true;
            info = status_of(dir);
        }
        if (!info || !S_ISDIR(info->st_mode) || info->st_uid != uid) {
            if (made) {
                rmdir(dir.c_str());  // a file system that ignores owners (FAT)
            }
            error = std::make_error_code(std::errc::permission_denied);
            return false;
        }
        return true;
    };
    const auto ready = [&error, &topdir](const std::filesystem::path& dir) {
        std::error_code made = make_directory(dir / "files");
        if (!made) {
            made = make_directory(dir / "info");
        }
        error = made;
        return made ? std::nullopt : std::optional<TrashDir>{{.path = dir, .relative_to = topdir}};
    };

    const std::filesystem::path shared = topdir / ".Trash";
    if (const std::optional<struct stat> info = status_of(shared);
        info && S_ISDIR(info->st_mode) && (info->st_mode & S_ISVTX) != 0) {
        const std::filesystem::path mine = shared / std::to_string(uid);
        if (usable(mine)) {
            return ready(mine);
        }
    }
    const std::filesystem::path mine = topdir / std::format(".Trash-{}", uid);
    if (usable(mine)) {
        return ready(mine);
    }
    return std::nullopt;
}

std::string escape_path(std::string_view path, bool keep_slash) {
    static constexpr std::string_view kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const char c : path) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                                c == '~' || (keep_slash && c == '/');
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(kDigits[byte >> 4U]);
            out.push_back(kDigits[byte & 0x0FU]);
        }
    }
    return out;
}

std::string trash_info(const std::filesystem::path& original, std::time_t when) {
    std::tm local{};
    localtime_r(&when, &local);
    std::array<char, 32> date{};
    std::strftime(date.data(), date.size(), "%Y-%m-%dT%H:%M:%S", &local);
    return std::format("[Trash Info]\nPath={}\nDeletionDate={}\n",
                       escape_path(original.string(), true), date.data());
}

std::string trash_name(std::string_view name, int id) {
    if (id == 1) {
        return std::string{name};
    }
    const std::size_t dot = name.find('.');
    if (dot == std::string_view::npos) {
        return std::format("{}.{}", name, id);
    }
    return std::format("{}.{}{}", name.substr(0, dot), id, name.substr(dot));
}

TransferResult run_trash(const TransferRequest& request, const core::CancelToken& token,
                         const TransferCallbacks& callbacks) {
    return Trasher{request, token, callbacks}.run();
}

const std::filesystem::path& trash_path() {
    static const std::filesystem::path kPath{"trash:///"};
    return kPath;
}

bool is_trash(const std::filesystem::path& path) { return path == trash_path(); }

std::vector<std::filesystem::path> mount_points(std::string_view mountinfo) {
    return parse_mount_points(mountinfo);
}

TrashSources TrashSources::from_environment() {
    return {.home = home_trash(), .tops = std::nullopt, .uid = geteuid()};
}

namespace {

std::optional<TrashSources>& sources_slot() {
    static std::optional<TrashSources> sources;
    return sources;
}

}  // namespace

void set_trash_sources(TrashSources sources) { sources_slot() = std::move(sources); }

const TrashSources& trash_sources() {
    std::optional<TrashSources>& sources = sources_slot();
    if (!sources) {
        sources = TrashSources::from_environment();
    }
    return *sources;
}

std::vector<TrashDir> existing_trashes(const TrashSources& sources) {
    std::vector<TrashDir> trashes;
    std::set<std::pair<dev_t, ino_t>> seen;
    const auto add = [&](const std::filesystem::path& dir, const std::filesystem::path& top) {
        const std::optional<struct stat> info = status_of(dir);
        if (!info || !seen.emplace(info->st_dev, info->st_ino).second) {
            return;
        }
        trashes.push_back({.path = dir, .relative_to = top});
    };
    if (!sources.home.empty() && usable_trash(sources.home, sources.uid)) {
        add(sources.home, {});
    }
    const std::vector<std::filesystem::path> tops =
        sources.tops ? *sources.tops : parse_mount_points(read_proc_file("/proc/self/mountinfo"));
    for (const std::filesystem::path& top : tops) {
        const std::filesystem::path shared = top / ".Trash";
        if (const std::optional<struct stat> info = status_of(shared);
            info && S_ISDIR(info->st_mode) && (info->st_mode & S_ISVTX) != 0) {
            const std::filesystem::path mine = shared / std::to_string(sources.uid);
            if (usable_trash(mine, sources.uid)) {
                add(mine, top);
            }
        }
        const std::filesystem::path mine = top / std::format(".Trash-{}", sources.uid);
        if (usable_trash(mine, sources.uid)) {
            add(mine, top);
        }
    }
    return trashes;
}

std::optional<TrashInfo> parse_trash_info(std::string_view text,
                                          const std::filesystem::path& relative_to) {
    std::optional<std::string> path;
    std::string deleted;
    bool in_group = false;
    bool first = true;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = text.substr(0, end);
        text.remove_prefix(std::min(end + 1, text.size()));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        if (line.starts_with('[')) {
            if (first && line != "[Trash Info]") {
                return std::nullopt;
            }
            in_group = line == "[Trash Info]";
            first = false;
            continue;
        }
        first = false;
        if (!in_group) {
            continue;
        }
        if (line.starts_with("Path=") && !path) {
            path = unescape(line.substr(5));
        } else if (line.starts_with("DeletionDate=") && deleted.empty()) {
            deleted = std::string{line.substr(13)};
        }
    }
    if (!path || path->empty()) {
        return std::nullopt;
    }
    std::filesystem::path original{*path};
    if (original.is_relative()) {
        original = relative_to / original;
    }
    return TrashInfo{.original = original.lexically_normal(), .deleted = std::move(deleted)};
}

std::vector<TrashedItem> list_trash(const TrashSources& sources) {
    std::vector<TrashedItem> items;
    for (const TrashDir& trash : existing_trashes(sources)) {
        for (Found& found : read_trash(trash.path)) {
            std::string name = found.info.original.filename().string();
            if (name.empty()) {
                continue;
            }
            items.push_back(
                {.name = std::move(name), .path = found.item, .info = std::move(found.info)});
        }
    }
    // The older keeps the name; the others take the first free numbered one.
    std::ranges::sort(items, [](const TrashedItem& a, const TrashedItem& b) {
        return std::tie(a.info.deleted, a.path) < std::tie(b.info.deleted, b.path);
    });
    std::unordered_set<std::string> taken;
    for (const TrashedItem& item : items) {
        taken.insert(item.name);
    }
    std::unordered_set<std::string> given;
    for (TrashedItem& item : items) {
        if (given.insert(item.name).second) {
            continue;
        }
        for (int id = 2;; ++id) {
            std::string candidate = trash_name(item.name, id);
            if (!taken.contains(candidate) && !given.contains(candidate)) {
                given.insert(candidate);
                item.name = std::move(candidate);
                break;
            }
        }
    }
    return items;
}

TransferResult run_restore(const TransferRequest& request, const core::CancelToken& token,
                           const TransferCallbacks& callbacks) {
    return Restorer{request, token, callbacks}.run();
}

std::filesystem::path info_file_of(const std::filesystem::path& item) {
    return item.parent_path().parent_path() / "info" /
           (item.filename().string() + std::string{kInfoSuffix});
}

std::vector<std::optional<TrashedGroup>> trashed_groups(
    std::span<const std::filesystem::path> items) {
    std::map<std::filesystem::path, std::vector<Found>> trashes;
    std::vector<std::optional<TrashedGroup>> groups;
    groups.reserve(items.size());
    for (const std::filesystem::path& item : items) {
        const std::filesystem::path trash = item.parent_path().parent_path();
        auto it = trashes.find(trash);
        if (it == trashes.end()) {
            it = trashes.emplace(trash, read_trash(trash)).first;
        }
        const auto found = std::ranges::find(it->second, item, &Found::item);
        if (found == it->second.end()) {
            groups.emplace_back();
            continue;
        }
        TrashedGroup group{.item = found->item, .info = found->info, .sidecars = {}};
        for (const Found& sidecar : trashed_sidecars(it->second, *found)) {
            group.sidecars.push_back(sidecar.item);
        }
        groups.emplace_back(std::move(group));
    }
    return groups;
}

void prune_directory_sizes(const std::filesystem::path& trash) {
    const std::filesystem::path sizes = trash / kSizes;
    if (!status_of(sizes)) {
        return;
    }
    add_directory_sizes(trash, {});
    if (const std::optional<struct stat> left = status_of(sizes); left && left->st_size == 0) {
        unlink(sizes.c_str());
    }
}

}  // namespace rummage::fs
