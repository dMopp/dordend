#include <malloc.h>
#include <sys/mman.h>
#include <unistd.h>

#include "backend.hpp"
#include "rummage/formats/formats.hpp"
#include "rummage/formats/text/read.hpp"
#include "rummage/formats/text/syntax.hpp"
#include "warm_up.hpp"

namespace rummage::isolate::backend {
namespace isolate = rummage::isolate;

// A memfd holding bytes, for the warm-up; -1 on an error.
int memfd_of(const isolate::WarmUpPicture& file) {
    const int fd = ::memfd_create(file.name, MFD_CLOEXEC);
    if (fd >= 0 && ::write(fd, file.data, file.size) != static_cast<ssize_t>(file.size)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Each kind of picture once, from a memfd, before the cage closes: whatever the libraries load
// on first use (libjxl's tables...) is loaded now. And one photo's information.
void warm_up() {
    const isolate::WarmUpPicture* photo = nullptr;
    for (std::size_t i = 0; i < isolate::kWarmUpCount; ++i) {
        const isolate::WarmUpPicture& file = isolate::kWarmUp[i];  // NOLINT: generated array
        if (std::string_view{file.name} == "shot.jpg") {
            photo = &file;
        }
        const int fd = memfd_of(file);
        if (fd >= 0) {
            static_cast<void>(rummage::formats::make_picture(fd, 64, 64));
            ::close(fd);
        }
    }
    if (photo != nullptr) {
        const int fd = memfd_of(*photo);
        if (fd >= 0) {
            static_cast<void>(rummage::formats::photo_info(fd));
            ::close(fd);
        }
    }
    ::malloc_trim(0);
}

picture::Loaded make_picture(int fd, std::uint32_t w, std::uint32_t h) {
    return formats::make_picture(fd, w, h);
}

picture::PhotoInfo photo_info(int fd) { return formats::photo_info(fd); }

text::Loaded read_text(int fd, const text::Limits& limits) {
    return formats::text::read(fd, limits);
}

void colour(text::Text& value, std::string_view name, std::string_view mime) {
    formats::text::colour(value, name, mime);
}
}  // namespace rummage::isolate::backend
