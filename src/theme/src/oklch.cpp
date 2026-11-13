#include "oklch.hpp"

#include <algorithm>
#include <cmath>

namespace rummage::theme {
namespace {

double from_linear(double v) {
    return v <= 0.0031308 ? v * 12.92 : (1.055 * std::pow(v, 1.0 / 2.4)) - 0.055;
}

struct Linear {
    double r{0.0};
    double g{0.0};
    double b{0.0};

    [[nodiscard]] bool in_gamut() const {
        constexpr double slack = 1e-4;
        return r >= -slack && r <= 1.0 + slack && g >= -slack && g <= 1.0 + slack && b >= -slack &&
               b <= 1.0 + slack;
    }
};

Linear to_linear_rgb(Lch lch) {
    const double a = lch.c * std::cos(lch.h);
    const double b = lch.c * std::sin(lch.h);
    const double l = std::pow(lch.l + (0.3963377774 * a) + (0.2158037573 * b), 3);
    const double m = std::pow(lch.l - (0.1055613458 * a) - (0.0638541728 * b), 3);
    const double s = std::pow(lch.l - (0.0894841775 * a) - (1.2914855480 * b), 3);
    return {(4.0767416621 * l) - (3.3077115913 * m) + (0.2309699292 * s),
            (-1.2684380046 * l) + (2.6097574011 * m) - (0.3413193965 * s),
            (-0.0041960863 * l) - (0.7034186147 * m) + (1.7076147010 * s)};
}

}  // namespace

double to_linear(float component) {
    const auto v = static_cast<double>(component);
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

Lch to_lch(Color color) {
    const double r = to_linear(color.r);
    const double g = to_linear(color.g);
    const double b = to_linear(color.b);
    const double l = std::cbrt((0.4122214708 * r) + (0.5363325363 * g) + (0.0514459929 * b));
    const double m = std::cbrt((0.2119034982 * r) + (0.6806995451 * g) + (0.1073969566 * b));
    const double s = std::cbrt((0.0883024619 * r) + (0.2817188376 * g) + (0.6299787005 * b));
    const double lightness = (0.2104542553 * l) + (0.7936177850 * m) - (0.0040720468 * s);
    const double a = (1.9779984951 * l) - (2.4285922050 * m) + (0.4505937099 * s);
    const double bb = (0.0259040371 * l) + (0.7827717662 * m) - (0.8086757660 * s);
    return {lightness, std::hypot(a, bb), std::atan2(bb, a)};
}

Color from_lch(Lch lch) {
    lch.l = std::clamp(lch.l, 0.0, 1.0);
    Linear rgb = to_linear_rgb(lch);
    if (!rgb.in_gamut()) {
        double low = 0.0;
        double high = lch.c;
        constexpr int steps = 24;
        for (int i = 0; i < steps; ++i) {
            const double mid = (low + high) / 2.0;
            (to_linear_rgb({lch.l, mid, lch.h}).in_gamut() ? low : high) = mid;
        }
        rgb = to_linear_rgb({lch.l, low, lch.h});
    }
    const auto channel = [](double v) {
        return static_cast<float>(from_linear(std::clamp(v, 0.0, 1.0)));
    };
    return {channel(rgb.r), channel(rgb.g), channel(rgb.b), 1.0F};
}

}  // namespace rummage::theme
