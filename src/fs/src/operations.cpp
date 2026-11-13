#include "rummage/fs/operations.hpp"
#include "rummage/core/i18n.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <fcntl.h>
#include <format>
#include <sys/stat.h>
#include <unistd.h>

namespace rummage::fs {
namespace {

std::error_code last_error() { return {errno, std::generic_category()}; }

bool taken(const std::filesystem::path& path) {
    struct stat info{};
    return lstat(path.c_str(), &info) == 0;
}

// rename(2) that fails with EEXIST instead of replacing to.
std::error_code rename_noreplace(const std::filesystem::path& from,
                                 const std::filesystem::path& to) {
    if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    if (errno != EINVAL && errno != ENOSYS) {
        return last_error();
    }
    // A file system without RENAME_NOREPLACE (some network and FUSE ones): check, then rename.
    // Another program could still slip a file in between; there is no better way there.
    if (taken(to)) {
        return std::make_error_code(std::errc::file_exists);
    }
    return std::rename(from.c_str(), to.c_str()) == 0 ? std::error_code{} : last_error();
}

// "photo.ORF" -> {"photo", ".ORF"}; no dot (or only a leading one) -> {name, ""}.
std::pair<std::string_view, std::string_view> split_extension(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) {
        return {name, {}};
    }
    return {name.substr(0, dot), name.substr(dot)};
}

}  // namespace

std::optional<std::string> name_problem(std::string_view name) {
    if (name.empty()) {
        return std::string{i18n::tr("A name cannot be empty")};
    }
    if (name == "." || name == "..") {
        return std::string{i18n::tr("A name cannot be . or ..")};
    }
    if (name.find('/') != std::string_view::npos) {
        return std::string{i18n::tr("A name cannot contain /")};
    }
    if (name.find('\0') != std::string_view::npos) {
        return std::string{i18n::tr("A name cannot contain a NUL character")};
    }
    if (name.size() > NAME_MAX) {
        return i18n::trf("The name is too long ({} bytes at most)", NAME_MAX);
    }
    return std::nullopt;
}

MadeFolder make_folder(const std::filesystem::path& directory, std::string_view base) {
    constexpr int tries = 10'000;
    for (int i = 1; i <= tries; ++i) {
        std::string name = i == 1 ? std::string{base} : std::format("{} {}", base, i);
        if (mkdir((directory / name).c_str(), 0777) == 0) {
            return {.name = std::move(name), .error = {}};
        }
        if (errno != EEXIST) {
            return {.name = {}, .error = last_error()};
        }
    }
    return {.name = {}, .error = std::make_error_code(std::errc::file_exists)};
}

MadeFile make_file(const std::filesystem::path& directory, std::string_view name,
                   const std::filesystem::path& from) {
    int source = -1;
    mode_t mode = 0666;  // less the umask, as any new file
    if (!from.empty()) {
        source = open(from.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat info{};
        if (source < 0 || fstat(source, &info) != 0) {
            const std::error_code error = last_error();
            if (source >= 0) {
                close(source);
            }
            return {.name = {}, .error = error};
        }
        mode = info.st_mode & 0777;
    }
    const auto [stem, extension] = split_extension(name);
    MadeFile made{.name = {}, .error = std::make_error_code(std::errc::file_exists)};
    constexpr int tries = 10'000;
    for (int i = 1; i <= tries; ++i) {
        std::string candidate =
            i == 1 ? std::string{name} : std::format("{} {}{}", stem, i, extension);
        const std::filesystem::path path = directory / candidate;
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            made.error = last_error();
            break;
        }
        std::error_code error;
        std::array<char, 65536> buffer{};
        for (ssize_t got = 0; source >= 0 && error == std::error_code{};) {
            got = read(source, buffer.data(), buffer.size());
            if (got == 0) {
                break;
            }
            if (got < 0) {
                if (errno != EINTR) {
                    error = last_error();
                }
                continue;
            }
            for (ssize_t done = 0; done < got && !error;) {
                const ssize_t wrote =
                    write(fd, buffer.data() + done, static_cast<std::size_t>(got - done));
                if (wrote < 0 && errno != EINTR) {
                    error = last_error();
                } else if (wrote > 0) {
                    done += wrote;
                }
            }
        }
        if (close(fd) != 0 && !error) {
            error = last_error();
        }
        if (error) {
            unlink(path.c_str());
            made.error = error;
        } else {
            made = {.name = std::move(candidate), .error = {}};
        }
        break;
    }
    if (source >= 0) {
        close(source);
    }
    return made;
}

std::vector<std::string> templates_in(const std::filesystem::path& directory) {
    std::vector<std::string> names;
    std::error_code error;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator{directory, error}) {
        std::string name = entry.path().filename().string();
        std::error_code ignored;
        if (name.starts_with('.') || name.ends_with('~') || !entry.is_regular_file(ignored)) {
            continue;
        }
        names.push_back(std::move(name));
    }
    std::ranges::sort(names);
    return names;
}

std::vector<std::string> sidecars_of(std::span<const std::string> names, std::string_view name) {
    constexpr std::string_view xmp = ".xmp";
    std::vector<std::string> sidecars;
    if (name.ends_with(xmp)) {
        return sidecars;
    }
    const auto [stem, extension] = split_extension(name);
    for (const std::string& candidate : names) {
        const std::string_view view{candidate};
        if (!view.ends_with(xmp)) {
            continue;
        }
        const std::string_view base = view.substr(0, view.size() - xmp.size());
        if (base == name) {
            sidecars.push_back(candidate);
            continue;
        }
        // A duplicate: stem, "_", two digits or more, the extension; unless a file has that name
        // ("IMG_1234.CR2" next to "IMG.CR2"): then the sidecar is that file's.
        if (extension.empty() || std::ranges::find(names, base) != names.end() ||
            !base.starts_with(stem) || !base.ends_with(extension) ||
            base.size() < stem.size() + extension.size() + 3 || base[stem.size()] != '_') {
            continue;
        }
        const std::string_view number =
            base.substr(stem.size() + 1, base.size() - stem.size() - 1 - extension.size());
        if (number.size() < 2 ||
            !std::ranges::all_of(number, [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        sidecars.push_back(candidate);
    }
    std::ranges::sort(sidecars);
    return sidecars;
}

std::string sidecar_name(std::string_view sidecar, std::string_view from, std::string_view to) {
    constexpr std::string_view xmp = ".xmp";
    if (sidecar == std::string{from} + std::string{xmp}) {
        return std::string{to} + std::string{xmp};
    }
    // A duplicate: the number between the stem and the extension stays.
    const auto [stem, extension] = split_extension(from);
    const auto [new_stem, new_extension] = split_extension(to);
    const std::string_view number = sidecar.substr(
        stem.size() + 1, sidecar.size() - xmp.size() - extension.size() - stem.size() - 1);
    return std::string{new_stem} + "_" + std::string{number} + std::string{new_extension} +
           std::string{xmp};
}

std::vector<std::pair<std::string, std::string>> sidecar_renames(std::span<const std::string> names,
                                                                 std::string_view from,
                                                                 std::string_view to) {
    std::vector<std::pair<std::string, std::string>> renames;
    if (from == to) {
        return renames;
    }
    for (std::string& sidecar : sidecars_of(names, from)) {
        std::string renamed = sidecar_name(sidecar, from, to);
        renames.emplace_back(std::move(sidecar), std::move(renamed));
    }
    return renames;
}

Renamed rename_entry(const std::filesystem::path& directory, const std::string& from,
                     const std::string& to) {
    Renamed result;
    if (from == to) {
        return result;
    }
    struct stat info{};
    if (lstat((directory / from).c_str(), &info) != 0) {
        result.error = last_error();
        result.problem = i18n::trf("Cannot rename {}: {}", from, result.error.message());
        return result;
    }

    std::vector<std::pair<std::string, std::string>> sidecars;
    if (S_ISREG(info.st_mode)) {
        std::vector<std::string> names;
        std::error_code ignored;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator{directory, ignored}) {
            names.push_back(entry.path().filename().string());
        }
        sidecars = sidecar_renames(names, from, to);
        for (const auto& [old_name, new_name] : sidecars) {
            if (taken(directory / new_name)) {
                result.error = std::make_error_code(std::errc::file_exists);
                result.problem = i18n::trf("{} already exists", new_name);
                return result;
            }
        }
    }

    result.error = rename_noreplace(directory / from, directory / to);
    if (result.error == std::errc::file_exists) {
        result.problem = i18n::trf("{} already exists", to);
        return result;
    }
    if (result.error) {
        result.problem = i18n::trf("Cannot rename {}: {}", from, result.error.message());
        return result;
    }
    for (const auto& [old_name, new_name] : sidecars) {
        if (const std::error_code error =
                rename_noreplace(directory / old_name, directory / new_name)) {
            if (result.problem.empty()) {
                result.problem =
                    i18n::trf("Renamed, but {} could not follow: {}", old_name, error.message());
            }
            continue;
        }
        ++result.sidecars;
    }
    return result;
}

}  // namespace rummage::fs
