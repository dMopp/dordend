#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "rummage/picture/image.hpp"

namespace rummage::test {
struct SvgDifference {
    int maximum{255};
    double fraction{1};

    [[nodiscard]] bool passes() const { return maximum <= 64 && fraction <= 0.01; }
};

inline SvgDifference svg_difference(const picture::Image& actual, const picture::Image& expected) {
    if (actual.empty() || actual.width != expected.width || actual.height != expected.height ||
        actual.rgba.size() != expected.rgba.size()) {
        return {};
    }
    SvgDifference result{0, 0};
    std::size_t different = 0;
    for (std::size_t i = 0; i < actual.rgba.size(); i += 4) {
        int pixel = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            // Compare visible contributions: straight RGB at alpha 0 or 1 magnifies harmless
            // antialiasing differences. Alpha itself is still compared without scaling.
            const int a =
                c == 3 ? actual.rgba[i + c] : (actual.rgba[i + c] * actual.rgba[i + 3] + 127) / 255;
            const int b = c == 3 ? expected.rgba[i + c]
                                 : (expected.rgba[i + c] * expected.rgba[i + 3] + 127) / 255;
            pixel = std::max(pixel, std::abs(a - b));
        }
        result.maximum = std::max(result.maximum, pixel);
        different += pixel > 24 ? 1U : 0U;
    }
    result.fraction =
        static_cast<double>(different) / (static_cast<double>(actual.width) * actual.height);
    return result;
}
}  // namespace rummage::test
