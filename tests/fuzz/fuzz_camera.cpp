// Fuzzes the camera files' previews (ROADMAP 2e-2): the TIFF structure, SubIFDs, maker notes
// (Olympus, Nikon, Samsung), RAF's header, RW2's JPEG, a JPEG's EXIF; the bytes in a memfd, as
// the preview helper gets them. Seeds: the first bytes of raw files (kept out of the repository).
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

#include "rummage/formats/image/preview.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const int fd = ::memfd_create("fuzz", MFD_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    if (::write(fd, data, size) == static_cast<ssize_t>(size)) {
        for (const std::uint32_t side : {160U, 4000U}) {
            const rummage::formats::image::EmbeddedPreview preview =
                rummage::formats::image::embedded_preview(fd, side);
            static_cast<void>(preview);
        }
    }
    ::close(fd);
    return 0;
}
