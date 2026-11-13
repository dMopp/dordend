#include <numbers>

#include "internal.hpp"

namespace rummage::svg {
namespace {
double linearize(double c) { return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4); }

double encode(double c) { return c <= .0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - .055; }

std::uint8_t byte(double c) { return channel(c * 255); }

void convert(Canvas& canvas, bool to_linear) {
    for (std::size_t i = 0; i < canvas.image.rgba.size(); i += 4) {
        const double alpha = canvas.image.rgba[i + 3] / 255.0;
        if (alpha == 0)
            continue;
        for (std::size_t c = 0; c < 3; ++c) {
            double value = std::clamp(canvas.image.rgba[i + c] / (255 * alpha), 0.0, 1.0);
            canvas.image.rgba[i + c] = byte((to_linear ? linearize(value) : encode(value)) * alpha);
        }
    }
}

void box_pass(const Canvas& input, Canvas& output, int radius, bool horizontal) {
    const int width = static_cast<int>(input.image.width),
              height = static_cast<int>(input.image.height);
    const int lines = horizontal ? height : width, extent = horizontal ? width : height;
    const unsigned divisor = static_cast<unsigned>(radius * 2 + 1);
    auto at = [&](int line, int pos) {
        return (horizontal ? static_cast<std::size_t>(line) * input.image.width +
                                 static_cast<unsigned>(pos)
                           : static_cast<std::size_t>(pos) * input.image.width +
                                 static_cast<unsigned>(line)) *
               4;
    };
    for (int line = 0; line < lines; ++line) {
        std::array<unsigned, 4> sum{};
        for (int p = 0; p <= radius && p < extent; ++p)
            for (std::size_t c = 0; c < 4; ++c)
                sum[c] += input.image.rgba[at(line, p) + c];
        for (int p = 0; p < extent; ++p) {
            for (std::size_t c = 0; c < 4; ++c)
                output.image.rgba[at(line, p) + c] =
                    static_cast<std::uint8_t>((sum[c] + divisor / 2) / divisor);
            if (p - radius >= 0)
                for (std::size_t c = 0; c < 4; ++c)
                    sum[c] -= input.image.rgba[at(line, p - radius) + c];
            if (p + radius + 1 < extent)
                for (std::size_t c = 0; c < 4; ++c)
                    sum[c] += input.image.rgba[at(line, p + radius + 1) + c];
        }
    }
}

std::array<int, 3> radii(double sigma) {
    const double ideal = std::sqrt(4 * sigma * sigma + 1);
    int lower = static_cast<int>(std::floor(ideal));
    if (lower % 2 == 0)
        --lower;
    lower = std::max(1, lower);
    const double n = (12 * sigma * sigma - 3 * lower * lower - 12 * lower - 9) / (-4 * lower - 4);
    const int count = std::clamp(static_cast<int>(std::lround(n)), 0, 3);
    std::array<int, 3> out{};
    for (int i = 0; i < 3; ++i)
        out[static_cast<std::size_t>(i)] = (i < count ? lower : lower + 2) / 2;
    return out;
}
}  // namespace

void colour_matrix(const Canvas& input, Canvas& output, const Node& node, bool linear) {
    std::array<double, 20> m{};
    m[0] = m[6] = m[12] = m[18] = 1;
    auto values = numbers(node.attr("values"));
    const auto type = node.attr("type");
    if (type.empty() || type == "matrix") {
        if (!values.empty()) {
            require(values.size() == 20);
            std::copy(values.begin(), values.end(), m.begin());
        }
    } else if (type == "saturate") {
        require(values.size() <= 1);
        const double s = values.empty() ? 1 : values[0];
        require(s >= 0);
        const std::array<double, 3> lum = {.213, .715, .072};
        for (std::size_t r = 0; r < 3; ++r)
            for (std::size_t c = 0; c < 3; ++c)
                m[r * 5 + c] = lum[c] * (1 - s) + (r == c ? s : 0);
    } else if (type == "hueRotate") {
        require(values.size() <= 1);
        const double a = (values.empty() ? 0 : values[0]) * std::numbers::pi / 180;
        const double c = std::cos(a), s = std::sin(a);
        m = {.213 + .787 * c - .213 * s,
             .715 - .715 * c - .715 * s,
             .072 - .072 * c + .928 * s,
             0,
             0,
             .213 - .213 * c + .143 * s,
             .715 + .285 * c + .140 * s,
             .072 - .072 * c - .283 * s,
             0,
             0,
             .213 - .213 * c - .787 * s,
             .715 - .715 * c + .715 * s,
             .072 + .928 * c + .072 * s,
             0,
             0,
             0,
             0,
             0,
             1,
             0};
    } else if (type == "luminanceToAlpha") {
        m = {};
        m[15] = .2125;
        m[16] = .7154;
        m[17] = .0721;
    } else
        throw Failure{};
    for (std::size_t i = 0; i < input.image.rgba.size(); i += 4) {
        Colour color{};
        color[3] = input.image.rgba[i + 3] / 255.0;
        if (color[3] > 0)
            for (std::size_t c = 0; c < 3; ++c) {
                color[c] = std::clamp(input.image.rgba[i + c] / (255 * color[3]), 0.0, 1.0);
                if (linear)
                    color[c] = linearize(color[c]);
            }
        Colour out{};
        for (std::size_t r = 0; r < 4; ++r) {
            double value = m[r * 5 + 4];
            for (std::size_t c = 0; c < 4; ++c)
                value += m[r * 5 + c] * color[c];
            require(std::isfinite(value));
            out[r] = std::clamp(value, 0.0, 1.0);
        }
        for (std::size_t c = 0; c < 3; ++c)
            output.image.rgba[i + c] = byte((linear ? encode(out[c]) : out[c]) * out[3]);
        output.image.rgba[i + 3] = byte(out[3]);
    }
}

void blur(const Canvas& input, Canvas& output, Canvas& scratch, double sx, double sy, bool linear) {
    output.image.rgba = input.image.rgba;
    if (sx == 0 && sy == 0)
        return;
    if (linear)
        convert(output, true);
    const auto rx = radii(sx), ry = radii(sy);
    for (std::size_t i = 0; i < 3; ++i) {
        box_pass(output, scratch, rx[i], true);
        box_pass(scratch, output, ry[i], false);
    }
    if (linear)
        convert(output, false);
}
}  // namespace rummage::svg
