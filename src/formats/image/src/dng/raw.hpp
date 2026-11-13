#pragma once

// A DNG's sensor data, read by rummage's own code (ROADMAP 2d-1): the main image (colour filter
// array or linear raw), uncompressed at 8 to 16 bits (packed when not 8 or 16) or as lossless
// JPEG, in strips or tiles, through the linearization table. What turns it into a picture (black
// and white levels, colour, demosaic) is the development's (2d-2); here the levels are only read.
// Rows come in bands, so a 100 MB sensor never has to be in memory whole.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../tiff/container.hpp"
#include "rummage/formats/image/decode.hpp"

namespace rummage::formats::image::dng {

inline constexpr std::uint64_t kCfa = 32803;
inline constexpr std::uint64_t kLinearRaw = 34892;

struct Raw {
    explicit Raw(tiff::Ifd main) : ifd{std::move(main)} {}

    tiff::Ifd ifd;  // the main image's IFD
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t samples{1};  // per pixel: 1 for a CFA, usually 3 for linear raw
    bool cfa{false};
    std::uint32_t bits{16};
    std::uint64_t compression{1};
    // The colour of each site of the CFA's repeating pattern, row by row: 0 red, 1 green,
    // 2 blue (3 cyan, 4 magenta, 5 yellow, 6 white).
    std::uint32_t pattern_width{0};
    std::uint32_t pattern_height{0};
    std::array<std::uint8_t, 64> pattern{};
    // Stored value -> linear value; empty for none.
    std::vector<std::uint16_t> linearization;
    // Black: a repeating pattern of rows x columns x samples, plus a delta for each column and
    // for each row of the active area.
    std::uint32_t black_rows{1};
    std::uint32_t black_columns{1};
    std::vector<double> black{0.0};
    std::vector<double> black_delta_h;
    std::vector<double> black_delta_v;
    std::array<double, 4> white{};  // per sample
    // The pixels that saw light: top, left, bottom, right.
    std::array<std::uint32_t, 4> active{};
    // The crop the camera meant, within the active area: x, y, width, height.
    std::array<double, 4> crop{};
};

// The main raw image of the DNG in container; nullopt with the reason (and its kind:
// unknown_format for a valid DNG this reader does not read) otherwise.
[[nodiscard]] std::optional<Raw> find_raw(const tiff::Container& container, std::string& error,
                                          Problem& problem);

// What read_rows gives: rows first_row to first_row + rows - 1, whole, width x samples values
// each, linearized. Returning false stops the reading.
using Band = std::function<bool(std::uint32_t first_row, std::uint32_t rows,
                                std::span<const std::uint16_t> values)>;

// Reads raw's rows top to bottom in bands (a strip, a row of tiles, or at most 64 rows of a
// larger strip). False with the reason in error when the data cannot be read; true when every
// band was given or band stopped it.
[[nodiscard]] bool read_rows(const Raw& raw, const Band& band, std::string& error);

// The whole image, width x height x samples values (tests and tools).
[[nodiscard]] std::optional<std::vector<std::uint16_t>> read_all(const Raw& raw,
                                                                 std::string& error);

}  // namespace rummage::formats::image::dng
