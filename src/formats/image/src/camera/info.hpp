#pragma once

// What a photo says about itself, read by rummage's own code (ROADMAP 2e-3, in place of exiv2):
// the EXIF of camera files and JPEGs, each value written as exiv2 printed it ("1/2000 s",
// "F5.6", "25.0 mm", "+7/10 EV", "Aperture priority"...), and the size of images that keep it in
// a header of their own (PNG, GIF, WebP). The sidecar's rating is not read here.

#include "../fd_file.hpp"
#include "rummage/picture/photo_info.hpp"

namespace rummage::formats::image::camera {

// file's information; an empty one for a file that says nothing rummage reads. Thread-safe.
[[nodiscard]] picture::PhotoInfo read_info(const detail::FdFile& file);

}  // namespace rummage::formats::image::camera
