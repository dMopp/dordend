#include "colour.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rummage::formats::image::dng {
namespace {

constexpr Matrix kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1};
// The profile connection space's white, D50.
constexpr Vector kD50{0.96422, 1.0, 0.82521};
// XYZ (D50) to linear sRGB, Bradford-adapted to sRGB's D65.
constexpr Matrix kD50ToSrgb{3.1338561, -1.6168667, -0.4906146, -0.9787684, 1.9161415,
                            0.0334540, 0.0719453,  -0.2289914, 1.4052427};
constexpr Matrix kBradford{0.8951, 0.2664, -0.1614, -0.7502, 1.7135,
                           0.0367, 0.0389, -0.0685, 1.0296};
constexpr double kMinTemperature = 2000.0;
constexpr double kMaxTemperature = 50000.0;

bool finite(const Matrix& m) {
    return std::ranges::all_of(m, [](double v) { return std::isfinite(v); });
}

std::optional<Matrix> matrix_tag(const tiff::Ifd& ifd, std::uint16_t tag) {
    const std::vector<double> values = ifd.reals(tag, 9);
    if (values.size() != 9) {
        return std::nullopt;
    }
    Matrix m{};
    std::ranges::copy(values, m.begin());
    if (!finite(m)) {
        return std::nullopt;
    }
    return m;
}

Matrix diagonal(const Vector& v) { return {v[0], 0, 0, 0, v[1], 0, 0, 0, v[2]}; }

Matrix mix(const Matrix& a, const Matrix& b, double weight) {
    Matrix out{};
    for (std::size_t i = 0; i < 9; ++i) {
        out.at(i) = (weight * a.at(i)) + ((1.0 - weight) * b.at(i));
    }
    return out;
}

Vector xy_to_xyz(double x, double y) { return {x / y, 1.0, (1.0 - x - y) / y}; }

// The two calibrations of a camera, and how to mix them for a white of some temperature.
struct Calibrations {
    Matrix colour1{};
    std::optional<Matrix> colour2;
    Matrix camera1{kIdentity};
    Matrix camera2{kIdentity};
    std::optional<Matrix> forward1;
    std::optional<Matrix> forward2;
    double temperature1{0.0};
    double temperature2{0.0};

    // The weight of the first calibration for a white of temperature t: by inverse temperature,
    // as the DNG SDK does; 1 when there is one calibration only or a temperature is unknown.
    [[nodiscard]] double weight(double t) const {
        if (!colour2 || temperature1 <= 0.0 || temperature2 <= 0.0 ||
            temperature1 == temperature2) {
            return 1.0;
        }
        const double w =
            ((1.0 / t) - (1.0 / temperature2)) / ((1.0 / temperature1) - (1.0 / temperature2));
        return std::clamp(w, 0.0, 1.0);
    }

    [[nodiscard]] Matrix colour(double w) const {
        return colour2 ? mix(colour1, *colour2, w) : colour1;
    }

    [[nodiscard]] Matrix camera(double w) const {
        return colour2 ? mix(camera1, camera2, w) : camera1;
    }

    [[nodiscard]] std::optional<Matrix> forward(double w) const {
        if (forward1 && forward2 && colour2) {
            return mix(*forward1, *forward2, w);
        }
        return forward1 ? forward1 : forward2;
    }
};

}  // namespace

Vector operator*(const Matrix& m, const Vector& v) {
    return {(m[0] * v[0]) + (m[1] * v[1]) + (m[2] * v[2]),
            (m[3] * v[0]) + (m[4] * v[1]) + (m[5] * v[2]),
            (m[6] * v[0]) + (m[7] * v[1]) + (m[8] * v[2])};
}

Matrix operator*(const Matrix& a, const Matrix& b) {
    Matrix out{};
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
            out.at((r * 3) + c) = (a.at(r * 3) * b.at(c)) + (a.at((r * 3) + 1) * b.at(3 + c)) +
                                  (a.at((r * 3) + 2) * b.at(6 + c));
        }
    }
    return out;
}

std::optional<Matrix> inverse(const Matrix& m) {
    const double a = (m[4] * m[8]) - (m[5] * m[7]);
    const double b = (m[5] * m[6]) - (m[3] * m[8]);
    const double c = (m[3] * m[7]) - (m[4] * m[6]);
    const double determinant = (m[0] * a) + (m[1] * b) + (m[2] * c);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) {
        return std::nullopt;
    }
    const double f = 1.0 / determinant;
    return Matrix{a * f, ((m[2] * m[7]) - (m[1] * m[8])) * f, ((m[1] * m[5]) - (m[2] * m[4])) * f,
                  b * f, ((m[0] * m[8]) - (m[2] * m[6])) * f, ((m[2] * m[3]) - (m[0] * m[5])) * f,
                  c * f, ((m[1] * m[6]) - (m[0] * m[7])) * f, ((m[0] * m[4]) - (m[1] * m[3])) * f};
}

double illuminant_temperature(std::uint64_t light_source) {
    switch (light_source) {
        case 17:  // standard light A
        case 3:   // tungsten
            return 2850.0;
        case 24:  // ISO studio tungsten
            return 3200.0;
        case 23:  // D50
            return 5000.0;
        case 20:  // D55
        case 1:   // daylight
        case 9:   // fine weather
        case 4:   // flash
            return 5500.0;
        case 21:  // D65
        case 10:  // cloudy
            return 6500.0;
        case 22:  // D75
        case 11:  // shade
            return 7500.0;
        case 12:  // daylight fluorescent
            return (5700.0 + 7100.0) * 0.5;
        case 13:  // day white fluorescent
            return (4600.0 + 5500.0) * 0.5;
        case 14:  // cool white fluorescent
        case 2:   // fluorescent
            return (3800.0 + 4500.0) * 0.5;
        case 15:  // white fluorescent
            return (3250.0 + 3800.0) * 0.5;
        case 16:  // warm white fluorescent
            return (2600.0 + 3250.0) * 0.5;
        default:
            return 0.0;
    }
}

double colour_temperature(double x, double y) {
    const double n = (x - 0.3320) / (0.1858 - y);
    const double t = (449.0 * n * n * n) + (3525.0 * n * n) + (6823.3 * n) + 5520.33;
    return std::isfinite(t) ? std::clamp(t, kMinTemperature, kMaxTemperature) : 5000.0;
}

std::optional<Colour> colour_of(const tiff::Ifd& ifd0, std::string& error) {
    Calibrations cal;
    const std::optional<Matrix> colour1 = matrix_tag(ifd0, tiff::tag::kColorMatrix1);
    if (!colour1) {
        error = "a DNG without a 3-colour ColorMatrix1";
        return std::nullopt;
    }
    cal.colour1 = *colour1;
    cal.colour2 = matrix_tag(ifd0, tiff::tag::kColorMatrix2);
    cal.camera1 = matrix_tag(ifd0, tiff::tag::kCameraCalibration1).value_or(kIdentity);
    cal.camera2 = matrix_tag(ifd0, tiff::tag::kCameraCalibration2).value_or(kIdentity);
    cal.forward1 = matrix_tag(ifd0, tiff::tag::kForwardMatrix1);
    cal.forward2 = matrix_tag(ifd0, tiff::tag::kForwardMatrix2);
    cal.temperature1 =
        illuminant_temperature(ifd0.number(tiff::tag::kCalibrationIlluminant1).value_or(0));
    cal.temperature2 =
        illuminant_temperature(ifd0.number(tiff::tag::kCalibrationIlluminant2).value_or(0));
    Matrix analog = kIdentity;
    if (const std::vector<double> balance = ifd0.reals(tiff::tag::kAnalogBalance, 3);
        balance.size() == 3 &&
        std::ranges::all_of(balance, [](double b) { return std::isfinite(b) && b > 0.0; })) {
        analog = diagonal({balance[0], balance[1], balance[2]});
    }
    // XYZ to the camera's values, for a white of temperature t.
    const auto xyz_to_camera = [&](double t) {
        const double w = cal.weight(t);
        return analog * cal.camera(w) * cal.colour(w);
    };

    // The neutral, and the temperature of the white it stands for (found by iterating: the
    // matrices depend on the temperature).
    Colour out;
    double temperature = 5000.0;
    const std::vector<double> neutral = ifd0.reals(tiff::tag::kAsShotNeutral, 3);
    const std::vector<double> white_xy = ifd0.reals(tiff::tag::kAsShotWhiteXy, 2);
    if (neutral.size() == 3 &&
        std::ranges::all_of(neutral, [](double n) { return std::isfinite(n) && n > 0.0; })) {
        out.neutral = {neutral[0], neutral[1], neutral[2]};
        for (int i = 0; i < 16; ++i) {
            const std::optional<Matrix> to_xyz = inverse(xyz_to_camera(temperature));
            if (!to_xyz) {
                break;
            }
            const Vector xyz = *to_xyz * out.neutral;
            const double sum = xyz[0] + xyz[1] + xyz[2];
            if (!(sum > 0.0)) {
                break;
            }
            const double next = colour_temperature(xyz[0] / sum, xyz[1] / sum);
            const bool settled = std::abs(next - temperature) < 1.0;
            temperature = next;
            if (settled) {
                break;
            }
        }
    } else {
        // A white given as xy (D65 when the file gives none).
        double x = 0.3127;
        double y = 0.3290;
        if (white_xy.size() == 2 && white_xy[0] > 0.0 && white_xy[1] > 0.0 &&
            white_xy[0] + white_xy[1] < 1.0) {
            x = white_xy[0];
            y = white_xy[1];
        }
        temperature = colour_temperature(x, y);
        out.neutral = xyz_to_camera(temperature) * xy_to_xyz(x, y);
    }
    const double largest = std::max({out.neutral[0], out.neutral[1], out.neutral[2]});
    if (!(largest > 0.0) || !std::ranges::all_of(out.neutral, [](double n) { return n > 0.0; })) {
        error = "a DNG whose neutral makes no sense";
        return std::nullopt;
    }
    for (double& n : out.neutral) {
        n /= largest;
    }
    out.temperature = temperature;

    // Camera to XYZ (D50).
    const double w = cal.weight(temperature);
    const Matrix calibration = analog * cal.camera(w);
    std::optional<Matrix> camera_to_pcs;
    if (const std::optional<Matrix> forward = cal.forward(w)) {
        // The forward matrix takes white-balanced reference camera values to D50.
        const std::optional<Matrix> uncalibrate = inverse(calibration);
        if (uncalibrate) {
            const Vector reference = *uncalibrate * out.neutral;
            if (std::ranges::all_of(reference, [](double r) { return r > 0.0; })) {
                camera_to_pcs =
                    *forward *
                    diagonal({1.0 / reference[0], 1.0 / reference[1], 1.0 / reference[2]}) *
                    *uncalibrate;
            }
        }
    } else if (const std::optional<Matrix> to_xyz = inverse(xyz_to_camera(temperature))) {
        // The colour matrix inverted, scaled so that the neutral has Y = 1, then adapted from
        // the neutral's white to D50 (Bradford).
        const Vector white = *to_xyz * out.neutral;
        if (white[1] > 0.0) {
            const Matrix scaled =
                diagonal({1.0 / white[1], 1.0 / white[1], 1.0 / white[1]}) * *to_xyz;
            const Vector source = kBradford * Vector{white[0] / white[1], 1.0, white[2] / white[1]};
            const Vector target = kBradford * kD50;
            const std::optional<Matrix> back = inverse(kBradford);
            if (back && std::ranges::all_of(source, [](double s) { return s > 0.0; })) {
                const Matrix adapt = *back *
                                     diagonal({target[0] / source[0], target[1] / source[1],
                                               target[2] / source[2]}) *
                                     kBradford;
                camera_to_pcs = adapt * scaled;
            }
        }
    }
    if (!camera_to_pcs || !finite(*camera_to_pcs)) {
        error = "a DNG whose colour matrices cannot be inverted";
        return std::nullopt;
    }
    out.camera_to_srgb = kD50ToSrgb * *camera_to_pcs;

    const std::vector<double> baseline = ifd0.reals(tiff::tag::kBaselineExposure, 1);
    const std::vector<double> offset = ifd0.reals(tiff::tag::kBaselineExposureOffset, 1);
    double stops = (baseline.empty() ? 0.0 : baseline[0]) + (offset.empty() ? 0.0 : offset[0]);
    if (!std::isfinite(stops)) {
        stops = 0.0;
    }
    out.exposure = std::exp2(std::clamp(stops, -4.0, 4.0));

    // A tone curve of at most 4096 points, rising from (0, y) to (1, y).
    const std::vector<double> curve = ifd0.reals(tiff::tag::kProfileToneCurve, 8192);
    if (curve.size() >= 4 && curve.size() % 2 == 0) {
        bool good = true;
        for (std::size_t i = 0; i < curve.size(); i += 2) {
            const double x = curve[i];
            const double y = curve[i + 1];
            good = good && std::isfinite(x) && std::isfinite(y) && x >= 0.0 && x <= 1.0 &&
                   (out.tone_curve.empty() || x > out.tone_curve.back()[0]);
            out.tone_curve.push_back({x, std::clamp(y, 0.0, 1.0)});
        }
        if (!good || out.tone_curve.front()[0] != 0.0 || out.tone_curve.back()[0] != 1.0) {
            out.tone_curve.clear();
        }
    }
    return out;
}

}  // namespace rummage::formats::image::dng
