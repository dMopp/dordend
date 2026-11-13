#pragma once

#include <cstdint>
#include <span>

#include "rummage/formats/image/decode.hpp"
#include "rummage/picture/load.hpp"

// The image family's picture: photos and images. The format is told by the file's first bytes,
// not its name; each format takes its own path (decoded whole, or from the preview a camera
// stored); a file that cannot be shown says why in one of a few plain reasons. Runs in the
// isolated helper (formats::make_picture), and in tests and development tools.
namespace rummage::formats::image {

using picture::Failure;
using picture::Loaded;
using picture::LoadRequest;

// Whether the first bytes of a file are an image this family reads (PNG, JPEG, GIF, WebP,
// JPEG XL, PGM/PPM, TIFF and the raw formats built like it, RAF, CR3, CRW, MRW, X3F).
[[nodiscard]] bool recognises(std::span<const std::uint8_t> head);

// The picture of request.path: from a preview the file carries when that is at least as large as
// asked (camera JPEGs, raw files), else from the whole image; a raw file without one large enough
// gives the largest it has. Scaled down to fit, then turned upright. Blocking. Never throws.
[[nodiscard]] Loaded load(const LoadRequest& request);

// The same from a file open for reading as fd (kept open, the caller's).
[[nodiscard]] Loaded load(int fd, std::uint32_t max_width, std::uint32_t max_height);

// How many times load() has looked for a file's previews, since the program started (for
// tests: formats that cannot carry one never reach it).
[[nodiscard]] std::uint64_t embedded_preview_reads() noexcept;

}  // namespace rummage::formats::image
