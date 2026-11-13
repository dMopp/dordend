#pragma once

// A DNG's colour, as the DNG specification (1.6, chapter 6) says to work it out (ROADMAP 2d-2):
// the camera's neutral (AsShotNeutral, or AsShotWhiteXY), the colour matrices and camera
// calibrations of the two calibration illuminants interpolated for the shot's white, the forward
// matrices when there are any (else the inverse colour matrix and a Bradford adaptation), to
// linear sRGB. Then the look: BaselineExposure, ProfileToneCurve (or a light curve of ours), the
// sRGB curve.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../tiff/container.hpp"

namespace rummage::formats::image::dng {

using Matrix = std::array<double, 9>;  // row by row
using Vector = std::array<double, 3>;

[[nodiscard]] Vector operator*(const Matrix& m, const Vector& v);
[[nodiscard]] Matrix operator*(const Matrix& a, const Matrix& b);
[[nodiscard]] std::optional<Matrix> inverse(const Matrix& m);

// The colour temperature (kelvin) of an EXIF LightSource code, as Adobe's DNG SDK takes it; 0
// for one without a temperature.
[[nodiscard]] double illuminant_temperature(std::uint64_t light_source);

// The correlated colour temperature of a CIE xy white (McCamy's approximation).
[[nodiscard]] double colour_temperature(double x, double y);

struct Colour {
    // The camera's neutral, its largest component 1: a sensor value equal to it is white.
    Vector neutral{1.0, 1.0, 1.0};
    // Camera values (0-1 above black) to linear sRGB.
    Matrix camera_to_srgb{};
    double temperature{0.0};  // of the shot's white, kelvin
    // 2^(BaselineExposure + BaselineExposureOffset).
    double exposure{1.0};
    // ProfileToneCurve: linear in, linear out, x rising from 0 to 1; empty when the file has none.
    std::vector<std::array<double, 2>> tone_curve;
};

// The colour of the DNG whose first IFD is ifd0, for a camera of 3 colours; nullopt with the
// reason when its colour tags are missing or make no sense.
[[nodiscard]] std::optional<Colour> colour_of(const tiff::Ifd& ifd0, std::string& error);

}  // namespace rummage::formats::image::dng
