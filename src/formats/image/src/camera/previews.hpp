#pragma once

// Where a camera file keeps its previews, read by rummage's own code (ROADMAP 2e-2, in place of
// exiv2's PreviewManager): the JPEGs and small TIFF images in a TIFF structure's IFDs and
// SubIFDs (NEF, CR2, ARW, ORF, RW2, SRW, DNG, 3FR, ERF, plain TIFF), in the maker notes that
// hold some (Olympus / OM System, Nikon, Samsung), in RW2's own JPEG and in RAF's; plus the size
// of the main image and its EXIF orientation. Nothing is decoded here and the file is only read
// where needed, with pread.

#include <cstdint>
#include <optional>
#include <vector>

#include "../fd_file.hpp"
#include "../tiff/container.hpp"

namespace rummage::formats::image::camera {

// The files told apart where the reading differs.
enum class Type : std::uint8_t {
    jpeg,
    tiff,  // TIFF and the raws built on it without a magic number of their own (NEF, ARW, DNG...)
    cr2,
    orf,
    rw2,
    raf,
};

struct Found {
    std::uint64_t offset{0};  // in the file
    std::uint64_t length{0};
    // An image of an IFD (to be copied out as a TIFF: tiff::extract), else a JPEG stream.
    std::optional<tiff::Ifd> ifd;
    // A TIFF image's size from its IFD; 0 for a JPEG (in its own header).
    std::uint32_t width{0};
    std::uint32_t height{0};
};

struct CameraFile {
    Type type{Type::tiff};
    std::vector<Found> previews;  // in the order found
    // The main image's size as stored (as exiv2 gave it, per format); 0 when unknown.
    std::uint32_t full_width{0};
    std::uint32_t full_height{0};
    std::uint8_t orientation{1};
};

// file's previews; nullopt for a file of none of these types. Without previews, only its type,
// size and orientation (fewer reads: the photo's information needs no more).
[[nodiscard]] std::optional<CameraFile> read(const detail::FdFile& file, bool previews = true);

}  // namespace rummage::formats::image::camera
