#include "rummage/fs/folder_icon.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rummage::fs {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// Folder metadata is untrusted. Bound reads and never wait for a FIFO writer.
std::string read_metadata(const std::filesystem::path& path) {
    constexpr std::size_t limit = 64U * 1024U;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return {};
    struct stat info{};
    std::string result;
    if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_size >= 0 &&
        static_cast<std::uint64_t>(info.st_size) <= limit) {
        result.resize(static_cast<std::size_t>(info.st_size));
        std::size_t offset = 0;
        while (offset < result.size()) {
            const auto count = ::read(fd, result.data() + offset, result.size() - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                result.clear();
                break;
            }
            offset += static_cast<std::size_t>(count);
        }
    }
    ::close(fd);
    return result;
}

std::filesystem::path normalized(const std::filesystem::path& path) {
    auto result = path.lexically_normal();
    if (!result.has_filename() && result != result.root_path())
        result = result.parent_path();
    return result;
}

}  // namespace

std::string parse_folder_icon(std::string_view text, const std::filesystem::path& folder) {
    bool desktop = false;
    std::string icon;
    while (!text.empty()) {
        const auto end = std::min(text.find('\n'), text.size());
        const auto line = trim(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[') {
            desktop = line == "[Desktop Entry]";
            continue;
        }
        const auto equals = line.find('=');
        if (!desktop || equals == std::string_view::npos || trim(line.substr(0, equals)) != "Icon")
            continue;
        const auto value = trim(line.substr(equals + 1));
        icon.clear();
        for (std::size_t i = 0; i < value.size(); ++i) {
            char c = value[i];
            if (c == '\\') {
                if (++i == value.size())
                    return {};
                switch (value[i]) {
                    case 's':
                        c = ' ';
                        break;
                    case 'n':
                        c = '\n';
                        break;
                    case 't':
                        c = '\t';
                        break;
                    case 'r':
                        c = '\r';
                        break;
                    case '\\':
                        c = '\\';
                        break;
                    default:
                        return {};
                }
            }
            if (c == '\0')
                return {};
            icon += c;
        }
    }
    if (icon.starts_with("./")) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(folder / icon, error);
        return error ? std::string{} : absolute.lexically_normal().string();
    }
    // Absolute file paths and theme names only; no URLs or other relative paths.
    if (!icon.empty() && icon.front() != '/' && icon.find('/') != std::string::npos)
        return {};
    return icon;
}

FolderIcons::FolderIcons(const PlaceSources& sources) {
    struct Kind {
        std::string_view key;
        std::string_view directory;
        std::string_view icon;
    };

    constexpr std::array kinds{
        Kind{"DESKTOP", "Desktop", "user-desktop"},
        Kind{"DOCUMENTS", "Documents", "folder-documents"},
        Kind{"DOWNLOAD", "Downloads", "folder-download"},
        Kind{"MUSIC", "Music", "folder-music"},
        Kind{"PICTURES", "Pictures", "folder-pictures"},
        Kind{"VIDEOS", "Videos", "folder-videos"},
        Kind{"PUBLICSHARE", "Public", "folder-publicshare"},
        Kind{"TEMPLATES", "Templates", "folder-templates"},
    };
    standard_.emplace_back(normalized(sources.home), "user-home");
    const auto config = sources.config_home / "user-dirs.dirs";
    std::error_code error;
    const bool configured = std::filesystem::exists(config, error);
    const auto dirs = parse_user_dirs(read_metadata(config), sources.home);
    for (const auto& kind : kinds) {
        auto path = sources.home / kind.directory;
        if (configured) {
            const auto it = std::ranges::find(dirs, kind.key, &UserDir::key);
            if (it == dirs.end())
                continue;
            path = it->path;
        }
        standard_.emplace_back(normalized(path), kind.icon);
    }
}

std::string FolderIcons::for_folder(const std::filesystem::path& folder) const {
    auto icon = parse_folder_icon(read_metadata(folder / ".directory"), folder);
    if (!icon.empty())
        return icon;
    return default_for_folder(folder);
}

std::string FolderIcons::default_for_folder(const std::filesystem::path& folder) const {
    std::error_code error;
    const auto path = normalized(std::filesystem::absolute(folder, error));
    if (error)
        return {};
    for (const auto& [standard, name] : standard_) {
        if (path == standard)
            return name;
    }
    return {};
}

}  // namespace rummage::fs
