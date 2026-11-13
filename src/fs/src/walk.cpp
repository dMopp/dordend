#include "rummage/fs/walk.hpp"

#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace rummage::fs {

void PathBatch::add(std::string_view relative, bool directory) {
    text_.append(relative);
    ends_.push_back(static_cast<std::uint32_t>(text_.size()));
    directories_.push_back(directory ? 1U : 0U);
}

std::string_view PathBatch::path(std::size_t index) const noexcept {
    const std::size_t begin = index == 0 ? 0 : ends_[index - 1];
    return std::string_view{text_}.substr(begin, ends_[index] - begin);
}

std::size_t PathBatch::bytes() const noexcept {
    return text_.capacity() + (ends_.capacity() * sizeof(std::uint32_t)) + directories_.capacity();
}

void PathBatch::shrink_to_fit() {
    text_.shrink_to_fit();
    ends_.shrink_to_fit();
    directories_.shrink_to_fit();
}

namespace {

// Closes a DIR* when it goes.
struct DirCloser {
    void operator()(DIR* dir) const noexcept { closedir(dir); }
};

struct Pending {
    std::string relative;  // "" for the root
    std::size_t depth{0};
};

}  // namespace

WalkResult walk(const std::filesystem::path& root, const WalkOptions& options,
                const core::CancelToken& token, const std::function<void(PathBatch)>& emit) {
    WalkResult result;
    struct stat root_stat{};
    if (::stat(root.c_str(), &root_stat) != 0 || !S_ISDIR(root_stat.st_mode)) {
        result.unreadable = 1;
        return result;
    }
    const dev_t device = root_stat.st_dev;
    const int root_fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) {
        result.unreadable = 1;
        return result;
    }

    PathBatch batch;
    const auto flush = [&] {
        if (!batch.empty()) {
            batch.shrink_to_fit();
            emit(std::exchange(batch, {}));
        }
    };
    std::deque<Pending> folders{{.relative = {}, .depth = 0}};
    std::string path;
    while (!folders.empty() && !result.truncated) {
        if (token.cancelled()) {
            result.cancelled = true;
            break;
        }
        const Pending folder = std::move(folders.front());
        folders.pop_front();
        const int fd = folder.relative.empty()
                           ? ::dup(root_fd)
                           : ::openat(root_fd, folder.relative.c_str(),
                                      O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) {
            ++result.unreadable;
            continue;
        }
        std::unique_ptr<DIR, DirCloser> dir{::fdopendir(fd)};
        if (!dir) {
            ::close(fd);
            ++result.unreadable;
            continue;
        }
        while (const dirent* entry = ::readdir(dir.get())) {
            const char* name = entry->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                continue;
            }
            if (!options.hidden && name[0] == '.') {
                continue;
            }
            path = folder.relative;
            if (!path.empty()) {
                path += '/';
            }
            path += name;
            bool directory = entry->d_type == DT_DIR;
            if (entry->d_type == DT_UNKNOWN) {  // some file systems do not say
                struct stat st{};
                directory = ::fstatat(::dirfd(dir.get()), name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                            S_ISDIR(st.st_mode);
            }
            batch.add(path, directory);
            if (++result.entries == options.max_entries) {
                result.truncated = true;
                break;
            }
            if (directory && folder.depth + 1 < options.max_depth) {
                // Entered only on the same file system.
                struct stat st{};
                if (::fstatat(::dirfd(dir.get()), name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                    st.st_dev == device) {
                    folders.push_back({.relative = path, .depth = folder.depth + 1});
                }
            }
            if (batch.size() >= options.batch) {
                flush();
            }
        }
    }
    flush();
    ::close(root_fd);
    return result;
}

}  // namespace rummage::fs
