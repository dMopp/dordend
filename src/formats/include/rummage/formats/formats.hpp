#pragma once

#include <cstdint>
#include <string_view>

#include "rummage/picture/load.hpp"
#include "rummage/picture/photo_info.hpp"

// The families of files rummage reads (ROADMAP 2): one module each under formats/ (image today;
// text, video and the others later), asked in order. Each says whether a file is its own from the
// first bytes, never from the name, and makes what rummage shows of it. Linked into the isolated
// helper only.
namespace rummage::formats {

// A family's name, for logs ("image").
[[nodiscard]] std::string_view family_of(int fd);

// The picture of the file open as fd (the caller's, kept open), made by the family that
// recognises it; files no family claims go to the image family, which knows camera raw files
// by more than their first bytes.
[[nodiscard]] picture::Loaded make_picture(int fd, std::uint32_t max_width,
                                           std::uint32_t max_height);

// What the file open as fd says about itself (EXIF); empty when its family has nothing to say.
[[nodiscard]] picture::PhotoInfo photo_info(int fd);

}  // namespace rummage::formats
