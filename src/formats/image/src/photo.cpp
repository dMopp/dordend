#include "rummage/formats/image/photo.hpp"

#include <fcntl.h>
#include <optional>
#include <unistd.h>

#include "camera/info.hpp"
#include "fd_file.hpp"

namespace rummage::formats::image {

PhotoInfo read_photo_info(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    PhotoInfo info = read_photo_info(fd);
    ::close(fd);
    return info;
}

PhotoInfo read_photo_info(int fd) {
    // No rating: rummage's own comes with ROADMAP 5; editors' sidecars are a plugin's (decided
    // with the author, 2026-11-08).
    if (const std::optional<detail::FdFile> file = detail::FdFile::of(fd)) {
        return camera::read_info(*file);
    }
    return {};
}

}  // namespace rummage::formats::image
