// Fuzzes the whole image family the way the preview helper runs it (ROADMAP 2c): the bytes in a
// memfd, through the formats registry, a picture and a photo's information.
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

#include "rummage/formats/formats.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const int fd = ::memfd_create("fuzz", MFD_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    if (::write(fd, data, size) == static_cast<ssize_t>(size)) {
        static_cast<void>(rummage::formats::make_picture(fd, 256, 256));
        static_cast<void>(rummage::formats::photo_info(fd));
    }
    ::close(fd);
    return 0;
}
