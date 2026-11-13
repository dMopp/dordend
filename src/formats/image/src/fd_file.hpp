#pragma once

// A file read through a descriptor it was opened as, with pread: the isolated helper gets files
// this way (it may not open any), and the same code serves rummage's own reads.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rummage::formats::image::detail {

class FdFile {
public:
    // fd stays the caller's: never closed here. nullopt when fstat fails.
    [[nodiscard]] static std::optional<FdFile> of(int fd);

    [[nodiscard]] int fd() const { return fd_; }

    [[nodiscard]] std::uint64_t size() const { return size_; }

    // Up to length bytes from offset (fewer at the end of the file); empty on an error.
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset, std::size_t length) const;

    // Reads into to from offset; how many bytes (fewer at the end of the file), or nullopt on an
    // error.
    [[nodiscard]] std::optional<std::size_t> read_into(std::uint64_t offset,
                                                       std::span<std::uint8_t> to) const;

    // Exactly length bytes from offset, or empty.
    [[nodiscard]] std::vector<std::uint8_t> read_exactly(std::uint64_t offset,
                                                         std::size_t length) const;

private:
    FdFile(int fd, std::uint64_t size) : fd_{fd}, size_{size} {}

    int fd_;
    std::uint64_t size_;
};

}  // namespace rummage::formats::image::detail
