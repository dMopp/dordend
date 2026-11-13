#pragma once

// A DNG developed to look at, not to edit (ROADMAP 2d-2): at half resolution or less (each 2x2
// block of the colour filter array, or of a linear raw, makes one pixel: sharp, quick, no
// demosaic artefacts; averaged by area to the size it will be shown at), black and
// white levels, white balance on the camera's neutral with clipped highlights kept white, the
// file's colour (colour.hpp) to sRGB, BaselineExposure, ProfileToneCurve or a light curve of ours,
// the default crop, the file's gain maps and lens warp (opcodes.hpp, 2d-4). Rows come two at a
// time from the sensor's bands: the sensor is never whole in memory.

#include <cstdint>
#include <optional>
#include <string>

#include "../tiff/container.hpp"
#include "rummage/formats/image/decode.hpp"

namespace rummage::formats::image::dng {

struct DevelopOptions {
    // The look (exposure and tone curve); false gives plain sRGB of the scene's colours (the
    // tests compare that with LibRaw's).
    bool look{true};
    // The file's opcodes (gain maps, lens warp); false leaves them out (LibRaw does too, so the
    // tests comparing with it do).
    bool opcodes{true};
    // The box the picture will be shown in (0: none): the half-resolution picture is averaged by
    // area to fit in it (as picture::scale_to_fit would, but on linear light), so that the colour
    // is worked out only for the pixels that will be shown.
    std::uint32_t max_width{0};
    std::uint32_t max_height{0};
};

struct Developed {
    Image image;  // the default crop at half its size or fitted in the box, as stored: not turned
    std::uint8_t orientation{1};
    std::uint32_t full_width{0};  // the default crop at full resolution
    std::uint32_t full_height{0};
};

// The DNG in container developed; nullopt with the reason (and its kind: unknown_format for a
// DNG this reader does not develop, which keeps its preview) otherwise.
[[nodiscard]] std::optional<Developed> develop(const tiff::Container& container,
                                               const DevelopOptions& options, std::string& error,
                                               Problem& problem);

}  // namespace rummage::formats::image::dng
