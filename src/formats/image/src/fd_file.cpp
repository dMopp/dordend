#include "fd_file.hpp"

#include <algorithm>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

namespace rummage::formats::image::detail {

std::optional<FdFile> FdFile::of(int fd) {
    struct stat info{};
    if (fd < 0 || ::fstat(fd, &info) != 0 || info.st_size < 0) {
        return std::nullopt;
    }
    return FdFile{fd, static_cast<std::uint64_t>(info.st_size)};
}

std::optional<std::size_t> FdFile::read_into(std::uint64_t offset,
                                             std::span<std::uint8_t> to) const {
    if (offset >= size_) {
        return 0;
    }
    const auto length =
        static_cast<std::size_t>(std::min<std::uint64_t>(to.size(), size_ - offset));
    std::size_t done = 0;
    while (done < length) {
        const ssize_t got =
            ::pread(fd_, to.data() + done, length - done, static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return std::nullopt;
        }
        if (got == 0) {
            break;  // shorter than fstat said: it shrank meanwhile
        }
        done += static_cast<std::size_t>(got);
    }
    return done;
}

std::vector<std::uint8_t> FdFile::read(std::uint64_t offset, std::size_t length) const {
    if (offset >= size_) {
        return {};
    }
    std::vector<std::uint8_t> data(
        static_cast<std::size_t>(std::min<std::uint64_t>(length, size_ - offset)));
    const std::optional<std::size_t> got = read_into(offset, data);
    if (!got) {
        return {};
    }
    data.resize(*got);
    return data;
}

std::vector<std::uint8_t> FdFile::read_exactly(std::uint64_t offset, std::size_t length) const {
    std::vector<std::uint8_t> data = read(offset, length);
    if (data.size() != length) {
        return {};
    }
    return data;
}

}  // namespace rummage::formats::image::detail
