#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace rummage::picture {

// What a photo says about itself (EXIF), written as exiv2 printed it ("1/2000 s", "F5.6"). Empty
// strings where the file does not say.
struct PhotoInfo {
    // The image's size upright (the EXIF orientation applied); 0 when unknown.
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::string camera;         // "OM-1", or "Canon EOS 5DS" (the maker added when missing)
    std::string lens;           // "OLYMPUS M.12-100mm F4.0"
    std::string exposure_time;  // "1/2000 s"
    std::string aperture;       // "F5.6"
    std::string iso;            // "200"
    std::string focal_length;   // "25.0 mm"
    std::string taken;          // "2026-08-31 16:34"
    std::string program;        // "Aperture priority"
    std::string exposure_bias;  // "+7/10 EV"
    std::string metering;       // "Spot"
    std::string flash;          // "No, compulsory"
    std::string white_balance;  // "Manual"
    std::string software;       // "Version 1.9"
    // rummage's own rating (ROADMAP 5, not yet read: nullopt): 1-5 stars, -1 rejected. Editors'
    // sidecars (darktable's FILE.xmp) are left to a plugin (decided with the author, 2026-11-08).
    std::optional<int> rating;
};

}  // namespace rummage::picture
