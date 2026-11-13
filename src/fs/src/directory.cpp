#include "rummage/fs/directory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "rummage/fs/folder_icon.hpp"

namespace rummage::fs {
namespace {

// Entries between cancellation checks.
constexpr std::size_t kCancelCheckInterval = 64;

EntryKind kind_of(mode_t mode) {
    if (S_ISREG(mode)) {
        return EntryKind::file;
    }
    if (S_ISDIR(mode)) {
        return EntryKind::directory;
    }
    return EntryKind::other;
}

struct DirCloser {
    void operator()(DIR* dir) const noexcept { closedir(dir); }
};

// Fills kind, size and time; false when the entry is gone.
bool stat_entry(int dir_fd, Entry& entry) {
    struct stat info{};
    if (fstatat(dir_fd, entry.name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
        return errno != ENOENT;  // keep it with unknown kind on other errors (EACCES, ...)
    }
    if (S_ISLNK(info.st_mode)) {
        entry.is_link = true;
        struct stat target{};
        if (fstatat(dir_fd, entry.name.c_str(), &target, 0) != 0) {
            entry.kind = EntryKind::other;  // dangling
            entry.modified_ns = (info.st_mtim.tv_sec * 1'000'000'000) + info.st_mtim.tv_nsec;
            return true;
        }
        info = target;
    }
    entry.kind = kind_of(info.st_mode);
    entry.size = static_cast<std::uint64_t>(info.st_size);
    entry.modified_ns = (info.st_mtim.tv_sec * 1'000'000'000) + info.st_mtim.tv_nsec;
    return true;
}

// The type when the kind or the name decides it (no file read), else the name's best guess.
void type_without_content(const MimeDatabase& db, Entry& entry) {
    switch (entry.kind) {
        case EntryKind::directory:
            entry.mime = MimeDatabase::kDirectory;
            return;
        case EntryKind::other:
            entry.mime = entry.is_link ? MimeDatabase::kSymlink : MimeDatabase::kSpecial;
            return;
        case EntryKind::file:
        case EntryKind::unknown:
            break;
    }
    if (entry.size == 0) {
        entry.mime = MimeDatabase::kZeroSize;
        return;
    }
    const MimeDatabase::NameMatch match = db.by_name(entry.name);
    (match.certain ? entry.mime : entry.name_guess) = match.mime;
}

}  // namespace

ReadResult read_directory(const std::filesystem::path& path, const core::CancelToken& token,
                          const ReadOptions& options,
                          const std::function<void(std::vector<Entry> chunk)>& on_chunk) {
    ReadResult result;
    const std::unique_ptr<DIR, DirCloser> dir{opendir(path.c_str())};
    if (!dir) {
        result.error = {errno, std::generic_category()};
        return result;
    }
    const int dir_fd = dirfd(dir.get());
    const FolderIcons icons;
    std::size_t chunk_size = std::max<std::size_t>(options.first_chunk, 1);
    std::vector<Entry> chunk;
    chunk.reserve(chunk_size);
    std::size_t seen = 0;
    for (;;) {
        errno = 0;
        const dirent* item = readdir(dir.get());
        if (item == nullptr) {
            if (errno != 0) {
                result.error = {errno, std::generic_category()};
            }
            break;
        }
        if (++seen % kCancelCheckInterval == 0 && token.cancelled()) {
            return result;
        }
        const std::string_view name{static_cast<const char*>(item->d_name)};
        if (name == "." || name == "..") {
            continue;
        }
        Entry entry{.name = std::string{name}};
        if (!stat_entry(dir_fd, entry)) {
            continue;
        }
        if (options.mime) {
            type_without_content(*options.mime, entry);
        }
        // Avoid probing zero-sized autofs placeholders (which could mount other homes).
        if (entry.is_directory() && entry.size != 0) {
            entry.folder_icon = icons.for_folder(path / entry.name);
        }
        chunk.push_back(std::move(entry));
        if (chunk.size() == chunk_size) {
            if (token.cancelled()) {
                return result;
            }
            result.count += chunk.size();
            on_chunk(std::move(chunk));
            chunk_size = std::min(chunk_size * 2, std::max(options.max_chunk, chunk_size));
            chunk = {};
            chunk.reserve(chunk_size);
        }
    }
    if (!chunk.empty() && !token.cancelled()) {
        result.count += chunk.size();
        on_chunk(std::move(chunk));
    }
    return result;
}

std::vector<std::optional<Entry>> read_entries(const std::filesystem::path& path,
                                               const std::vector<std::string>& names,
                                               const ReadOptions& options) {
    std::vector<std::optional<Entry>> result(names.size());
    const int dir_fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        return result;
    }
    const FolderIcons icons;
    for (std::size_t i = 0; i < names.size(); ++i) {
        Entry entry{.name = names[i]};
        if (!stat_entry(dir_fd, entry)) {
            continue;  // gone
        }
        if (options.mime) {
            type_without_content(*options.mime, entry);
        }
        if (entry.is_directory() && entry.size != 0) {
            entry.folder_icon = icons.for_folder(path / entry.name);
        }
        result[i] = std::move(entry);
    }
    close(dir_fd);
    return result;
}

core::CancelToken load_directory(core::WorkerPool& pool, std::filesystem::path path,
                                 std::function<void(std::vector<Entry> chunk)> on_chunk,
                                 std::function<void(const ReadResult& result)> on_done,
                                 ReadOptions options) {
    return pool.submit_streaming(
        core::Priority::visible,
        [path = std::move(path), options](const core::CancelToken& token, auto& emit) {
            return read_directory(path, token, options,
                                  [&emit](std::vector<Entry> chunk) { emit(std::move(chunk)); });
        },
        [on_chunk = std::move(on_chunk)](std::vector<Entry> chunk) { on_chunk(std::move(chunk)); },
        [on_done = std::move(on_done)](const ReadResult& result) { on_done(result); });
}

}  // namespace rummage::fs
