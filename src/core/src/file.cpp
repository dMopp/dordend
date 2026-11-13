#include "rummage/core/file.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <ios>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace rummage::core {
namespace {

constexpr std::string_view kHex = "0123456789ABCDEF";

// GLib leaves these unescaped in the path of a file URI (g_filename_to_uri, checked with gio).
bool unescaped(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    return std::string_view{"!&'()*+,=:@$~_-./"}.find(static_cast<char>(c)) !=
           std::string_view::npos;
}

std::error_code last_error() { return {errno, std::generic_category()}; }

}  // namespace

std::string file_uri(const std::filesystem::path& absolute) {
    std::string uri = "file://";
    for (const char ch : absolute.native()) {
        const auto c = static_cast<unsigned char>(ch);
        if (unescaped(c)) {
            uri.push_back(ch);
        } else {
            uri.push_back('%');
            uri.push_back(kHex[c >> 4]);
            uri.push_back(kHex[c & 0x0F]);
        }
    }
    return uri;
}

std::error_code write_file_atomically(const std::filesystem::path& path, std::string_view text,
                                      bool replace) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return error;
    }
    // The file keeps its permissions; a new one gets rw-r--r--.
    struct stat old{};
    const mode_t mode = ::stat(path.c_str(), &old) == 0 ? (old.st_mode & 07777) : 0644;
    std::string temporary = (path.parent_path() / ("." + path.filename().string() + ".XXXXXX"));
    const int fd = ::mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0) {
        return last_error();
    }
    const auto fail = [&](std::error_code why) {
        ::close(fd);
        ::unlink(temporary.c_str());
        return why;
    };
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t put = ::write(fd, text.data() + written, text.size() - written);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(last_error());
        }
        written += static_cast<std::size_t>(put);
    }
    if (::fchmod(fd, mode) != 0 || ::fsync(fd) != 0) {
        return fail(last_error());
    }
    if (::close(fd) != 0) {
        ::unlink(temporary.c_str());
        return last_error();
    }
    if (!replace) {
        // A hard link publishes the complete file atomically and cannot replace an entry.
        const int linked = ::link(temporary.c_str(), path.c_str());
        const std::error_code why =
            linked == 0 || errno == EEXIST ? std::error_code{} : last_error();
        ::unlink(temporary.c_str());
        return why;
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const std::error_code why = last_error();
        ::unlink(temporary.c_str());
        return why;
    }
    return {};
}

TextFile read_text_file(const std::filesystem::path& path) {
    TextFile result;
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return result;
    }
    result.exists = true;
    if (error) {
        result.error = error.message();
        return result;
    }
    if (status.type() != std::filesystem::file_type::regular) {
        result.error = "not a regular file";
        return result;
    }
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        result.error = "cannot open: " + std::error_code{errno, std::generic_category()}.message();
        return result;
    }
    // Sized read: istreambuf_iterator trips GCC 14's -Wnull-dereference inside libstdc++.
    std::string text(static_cast<std::size_t>(std::filesystem::file_size(path, error)), '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (error || file.gcount() != static_cast<std::streamsize>(text.size())) {
        result.error = "read error";
        return result;
    }
    result.text = std::move(text);
    return result;
}

std::string migrate_nordend_file(const std::filesystem::path& destination) {
    if (destination.empty() || destination.parent_path().filename() != "rummage")
        return {};
    std::error_code error;
    const auto status = std::filesystem::symlink_status(destination, error);
    if (status.type() != std::filesystem::file_type::not_found)
        return error ? error.message() : std::string{};
    const auto source =
        destination.parent_path().parent_path() / "nordend" / destination.filename();
    const auto legacy = read_text_file(source);
    if (!legacy.exists)
        return {};
    if (!legacy.error.empty())
        return source.string() + ": " + legacy.error;
    error = write_file_atomically(destination, legacy.text, false);
    return error ? error.message() : std::string{};
}

}  // namespace rummage::core
