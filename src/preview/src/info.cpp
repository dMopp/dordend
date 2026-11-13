#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "rummage/preview/preview.hpp"

namespace rummage::preview {
namespace {

constexpr std::size_t kNameBuffer = 4096;     // getpwuid_r / getgrgid_r scratch space
constexpr std::uint64_t kCancelCheck = 4096;  // entries counted between looks at the token

char type_letter(mode_t mode) {
    if (S_ISDIR(mode)) {
        return 'd';
    }
    if (S_ISLNK(mode)) {
        return 'l';
    }
    if (S_ISCHR(mode)) {
        return 'c';
    }
    if (S_ISBLK(mode)) {
        return 'b';
    }
    if (S_ISFIFO(mode)) {
        return 'p';
    }
    if (S_ISSOCK(mode)) {
        return 's';
    }
    return '-';
}

// As ls -l: "drwxr-xr-x", with s/S for setuid and setgid, t/T for sticky.
std::string permissions(mode_t mode) {
    std::string out(10, '-');
    out[0] = type_letter(mode);
    constexpr std::array<mode_t, 9> bits{S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
                                         S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
    constexpr std::string_view letters = "rwxrwxrwx";
    for (std::size_t i = 0; i < bits.size(); ++i) {
        if ((mode & bits.at(i)) != 0) {
            out[i + 1] = letters[i];
        }
    }
    const auto special = [&](std::size_t at, mode_t bit, char set, char set_no_exec) {
        if ((mode & bit) != 0) {
            out[at] = out[at] == 'x' ? set : set_no_exec;
        }
    };
    special(3, S_ISUID, 's', 'S');
    special(6, S_ISGID, 's', 'S');
    special(9, S_ISVTX, 't', 'T');
    return out;
}

std::string user_name(uid_t uid) {
    passwd entry{};
    passwd* found = nullptr;
    std::vector<char> buffer(kNameBuffer);
    if (::getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr) {
        return entry.pw_name;
    }
    return std::to_string(uid);
}

std::string group_name(gid_t gid) {
    group entry{};
    group* found = nullptr;
    std::vector<char> buffer(kNameBuffer);
    if (::getgrgid_r(gid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr) {
        return entry.gr_name;
    }
    return std::to_string(gid);
}

}  // namespace

Info read_info(const std::filesystem::path& path, const core::CancelToken& cancel) {
    Info info;
    struct stat link{};
    if (::lstat(path.c_str(), &link) != 0) {
        info.error = std::strerror(errno);  // NOLINT(concurrency-mt-unsafe): glibc's is safe
        return info;
    }
    struct stat target = link;
    if (S_ISLNK(link.st_mode)) {
        std::error_code error;
        info.link_target = std::filesystem::read_symlink(path, error).string();
        if (::stat(path.c_str(), &target) != 0) {
            target = link;  // dangling: the link's own
        }
    }
    struct statx birth{};
    // NOLINTNEXTLINE(hicpp-signed-bitwise): flags as the kernel defines them
    if (::statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME, &birth) == 0 &&
        (birth.stx_mask & STATX_BTIME) != 0) {
        info.created_ns = (static_cast<std::int64_t>(birth.stx_btime.tv_sec) * 1'000'000'000) +
                          birth.stx_btime.tv_nsec;
    }
    info.permissions = permissions(target.st_mode);
    info.owner = user_name(target.st_uid);
    info.group = group_name(target.st_gid);
    if (S_ISDIR(target.st_mode)) {
        std::error_code error;
        std::filesystem::directory_iterator it{path, error};
        if (!error) {
            std::uint64_t count = 0;
            std::uint64_t hidden = 0;
            for (; it != std::filesystem::directory_iterator{}; it.increment(error)) {
                if (error || (++count % kCancelCheck == 0 && cancel.cancelled())) {
                    break;
                }
                if (it->path().filename().native().starts_with('.')) {
                    ++hidden;
                }
            }
            if (!error) {
                info.items = count;
                info.hidden_items = hidden;
            }
        }
    }
    return info;
}

}  // namespace rummage::preview
