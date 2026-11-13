#pragma once

// A DNG's opcodes (ROADMAP 2d-4): the corrections a camera leaves for the reader to apply, in
// three lists (1 on the data as stored, 2 on the linear values before demosaic, 3 after it).
// rummage applies the two its cameras use: GainMap (vignetting and colour shading) and
// WarpRectilinear (lens distortion). Opcodes marked optional, or as skippable for previews, are
// skipped; bad pixel fixes are skipped too (at half size or less a site is averaged with three
// others at least). Any other opcode the file requires stops the development: the previews are
// shown instead, as the DNG specification asks of a reader that cannot apply one.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../tiff/container.hpp"

namespace rummage::formats::image::dng {

// Gains over a grid of points, multiplying the planes of an area of the image (every
// row_pitch-th row and column_pitch-th column from its top left).
struct GainMap {
    // Top, left, bottom, right, in the image's pixels (the active area's).
    std::array<std::uint32_t, 4> area{};
    std::uint32_t plane{0};
    std::uint32_t planes{1};
    std::uint32_t row_pitch{1};
    std::uint32_t column_pitch{1};
    std::uint32_t points_v{1};
    std::uint32_t points_h{1};
    std::uint32_t map_planes{1};
    // Where the grid's points are, relative to the image's height and width.
    double spacing_v{1.0};
    double spacing_h{1.0};
    double origin_v{0.0};
    double origin_h{0.0};
    std::vector<float> gains;  // points_v x points_h x map_planes, row by row

    // Where a position relative to the image's height (or width) falls in the grid: the point
    // before it and the fraction of the way to the next one (the edge point beyond the grid).
    struct Step {
        std::uint32_t first{0};
        double fraction{0.0};
    };

    [[nodiscard]] Step step_v(double v) const;
    [[nodiscard]] Step step_h(double h) const;

    [[nodiscard]] float gain(std::uint32_t row, std::uint32_t column, std::uint32_t p) const {
        return gains[(((std::size_t{row} * points_h) + column) * map_planes) +
                     std::min(p, map_planes - 1)];
    }

    // The gain at (v, h), relative to the image's height and width, for map plane p (the last
    // one for planes beyond it): interpolated between the four nearest points.
    [[nodiscard]] float at(double v, double h, std::uint32_t p) const;
};

// WarpRectilinear, with one plane's coefficients (green's when there are three: the planes
// differ by under half a pixel at the sizes rummage develops to).
struct Warp {
    std::array<double, 4> radial{1.0, 0.0, 0.0, 0.0};
    std::array<double, 2> tangential{};
    // The optical centre, relative to the image's width and height.
    double centre_x{0.5};
    double centre_y{0.5};
};

// The warp on an image width x height: where its point (x, y) takes its value from (pixels
// from the top left corner: pixel i's centre is at i + 0.5).
class WarpMap {
public:
    WarpMap(const Warp& warp, double width, double height);

    [[nodiscard]] std::array<double, 2> source(double x, double y) const;

private:
    Warp warp_;
    double cx_;
    double cy_;
    double reach_;
};

struct Opcodes {
    std::vector<GainMap> before_demosaic;  // list 2
    std::vector<GainMap> after_demosaic;   // list 3, all before the warp
    std::optional<Warp> warp;              // list 3

    [[nodiscard]] bool empty() const {
        return before_demosaic.empty() && after_demosaic.empty() && !warp;
    }
};

// Opcode list number (1, 2 or 3) as stored in the tag (always big-endian), added to opcodes.
// False with the reason when an opcode is damaged or must be applied and is not known here.
[[nodiscard]] bool read_list(std::span<const std::uint8_t> bytes, int number, Opcodes& opcodes,
                             std::string& error);

// The three lists of the raw image's IFD.
[[nodiscard]] std::optional<Opcodes> read_opcodes(const tiff::Ifd& ifd, std::string& error);

}  // namespace rummage::formats::image::dng
