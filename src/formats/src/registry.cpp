#include <array>
#include <cerrno>
#include <cstddef>
#include <span>
#include <unistd.h>

#include "rummage/formats/formats.hpp"
#include "rummage/formats/image/load.hpp"
#include "rummage/formats/image/photo.hpp"

namespace rummage::formats {
namespace {

// Enough for every family's magic numbers.
constexpr std::size_t kHead = 64;

struct Family {
    std::string_view name;
    bool (*recognises)(std::span<const std::uint8_t> head);
    picture::Loaded (*picture)(int fd, std::uint32_t max_width, std::uint32_t max_height);
    picture::PhotoInfo (*info)(int fd);  // nullptr: nothing to say
};

// In the order they are asked. A new family: its module under formats/, one line here.
constexpr std::array kFamilies{
    Family{
        .name = "image",
        .recognises = image::recognises,
        .picture = [](int fd, std::uint32_t w, std::uint32_t h) { return image::load(fd, w, h); },
        .info = [](int fd) { return image::read_photo_info(fd); }},
};

// The family a file's first bytes belong to; the image family when none claims them.
const Family& family_for(int fd) {
    std::array<std::uint8_t, kHead> head{};
    ssize_t got = ::pread(fd, head.data(), head.size(), 0);
    while (got < 0 && errno == EINTR) {
        got = ::pread(fd, head.data(), head.size(), 0);
    }
    const std::span<const std::uint8_t> bytes{
        head.data(), got > 0 ? static_cast<std::size_t>(got) : std::size_t{0}};
    for (const Family& family : kFamilies) {
        if (family.recognises(bytes)) {
            return family;
        }
    }
    return kFamilies.front();
}

}  // namespace

std::string_view family_of(int fd) { return family_for(fd).name; }

picture::Loaded make_picture(int fd, std::uint32_t max_width, std::uint32_t max_height) {
    return family_for(fd).picture(fd, max_width, max_height);
}

picture::PhotoInfo photo_info(int fd) {
    const Family& family = family_for(fd);
    return family.info != nullptr ? family.info(fd) : picture::PhotoInfo{};
}

}  // namespace rummage::formats
