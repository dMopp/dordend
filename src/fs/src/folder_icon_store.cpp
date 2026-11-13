#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "rummage/core/file.hpp"
#include "rummage/fs/folder_icon.hpp"

namespace rummage::fs {
namespace {

struct Read {
    std::string text;
    std::string error;
};

Read read_at(int directory) {
    const int fd =
        ::openat(directory, ".directory", O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0)
        return {{},
                errno == ENOENT ? std::string{}
                                : std::error_code{errno, std::generic_category()}.message()};
    struct stat info{};
    Read result;
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        info.st_size > 65536) {
        result.error = ".directory must be a regular file no larger than 64 KiB";
    } else {
        result.text.resize(static_cast<std::size_t>(info.st_size));
        std::size_t at = 0;
        while (at < result.text.size()) {
            const auto count = ::read(fd, result.text.data() + at, result.text.size() - at);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                result.error = "Could not read .directory";
                break;
            }
            at += static_cast<std::size_t>(count);
        }
    }
    ::close(fd);
    return result;
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    return first == std::string_view::npos
               ? std::string_view{}
               : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string escaped(std::string_view icon) {
    std::string out;
    for (char c : icon) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case ' ':
                out += "\\s";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += c;
        }
    }
    return out;
}

}  // namespace

std::string update_folder_icon_text(std::string_view text, std::string_view icon) {
    const std::string newline = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    std::string out;
    bool desktop = false;
    bool written = false;
    while (!text.empty()) {
        const auto end = std::min(text.find('\n'), text.size());
        const auto raw = text.substr(0, std::min(end + 1, text.size()));
        const auto line = trim(raw);
        if (line.starts_with('[')) {
            desktop = line == "[Desktop Entry]";
            out += raw;
            if (desktop && !written && !icon.empty()) {
                if (!out.ends_with('\n'))
                    out += newline;
                out += "Icon=" + escaped(icon) + newline;
                written = true;
            }
        } else if (!(desktop && trim(line.substr(0, line.find('='))) == "Icon" &&
                     line.find('=') != std::string_view::npos)) {
            out += raw;
        }
        text.remove_prefix(raw.size());
    }
    if (!written && !icon.empty()) {
        if (!out.empty() && !out.ends_with('\n'))
            out += newline;
        out += "[Desktop Entry]" + newline + "Icon=" + escaped(icon) + newline;
    }
    return out;
}

FolderIconSetting read_folder_icon_setting(const std::filesystem::path& folder) {
    const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return {{}, std::error_code{errno, std::generic_category()}.message()};
    const auto read = read_at(fd);
    ::close(fd);
    return {parse_folder_icon(read.text, folder), read.error};
}

std::string save_folder_icon(const std::filesystem::path& folder, std::string_view icon) {
    if (icon.find('\0') != std::string_view::npos || icon.size() > 4096)
        return "Invalid icon name or path";
    // Anchor the write to this open directory. A concurrent rename cannot redirect it or
    // cause write_file_atomically to recreate a folder that was deleted meanwhile.
    const int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return std::error_code{errno, std::generic_category()}.message();
    const auto read = read_at(fd);
    if (!read.error.empty()) {
        ::close(fd);
        return read.error;
    }
    const auto text = update_folder_icon_text(read.text, icon);
    std::string error;
    if (text.size() > 65536) {
        error = ".directory would exceed 64 KiB";
    } else if (text != read.text) {
        const auto path =
            std::filesystem::path{"/proc/self/fd"} / std::to_string(fd) / ".directory";
        const auto result = core::write_file_atomically(path, text);
        if (result)
            error = result.message();
    }
    ::close(fd);
    return error;
}

}  // namespace rummage::fs
