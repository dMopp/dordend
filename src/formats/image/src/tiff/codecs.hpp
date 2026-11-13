#pragma once

// The compressions and predictors of TIFF that rummage reads (ROADMAP 2c). Each decompressor is
// told how many bytes the strip or tile holds and never writes more: data that ends early is
// padded with zeros (as libtiff does, with a warning), data that is broken gives nullopt.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rummage::formats::image::tiff {

inline constexpr std::uint16_t kNone = 1;
inline constexpr std::uint16_t kLzw = 5;
inline constexpr std::uint16_t kDeflate = 8;
inline constexpr std::uint16_t kDeflateOld = 32946;  // Adobe's code for the same
inline constexpr std::uint16_t kPackBits = 32773;

// Whether a compression is one of the above.
[[nodiscard]] bool supported_compression(std::uint64_t compression) noexcept;

// The expected bytes of a strip or tile from its compressed bytes.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> unpack(std::uint64_t compression,
                                                              std::span<const std::uint8_t> in,
                                                              std::size_t expected);

// Predictor 2, horizontal differencing, undone in place on rows of row_samples samples of
// bytes_per_sample bytes (1, 2, 4, 8) already in this machine's byte order; stride is the
// samples per pixel (1 for planar data).
void undo_horizontal(std::span<std::uint8_t> data, std::size_t row_samples, std::size_t stride,
                     std::size_t bytes_per_sample);

// Predictor 3, floating point (Adobe's TIFF technical note 3), undone on rows of row_samples
// samples of bytes_per_sample bytes (2, 4, 8): the result is in this machine's byte order.
void undo_floating_point(std::span<std::uint8_t> data, std::size_t row_samples, std::size_t stride,
                         std::size_t bytes_per_sample);

}  // namespace rummage::formats::image::tiff
