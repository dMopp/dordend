#include "rummage/picture/scale.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rummage::picture {
namespace {

// The input pixels one output pixel covers, with how much of each.
struct Span {
    std::size_t first{0};
    std::vector<float> weights;  // sum to 1
};

std::vector<Span> spans(std::size_t in, std::size_t out) {
    std::vector<Span> result(out);
    const double ratio = static_cast<double>(in) / static_cast<double>(out);
    for (std::size_t o = 0; o < out; ++o) {
        const double from = static_cast<double>(o) * ratio;
        const double to = static_cast<double>(o + 1) * ratio;
        Span& span = result[o];
        span.first = static_cast<std::size_t>(from);
        const auto last = std::min(in, static_cast<std::size_t>(std::ceil(to)));
        for (std::size_t i = span.first; i < last; ++i) {
            const double covered =
                std::min(to, static_cast<double>(i + 1)) - std::max(from, static_cast<double>(i));
            span.weights.push_back(static_cast<float>(covered / ratio));
        }
    }
    return result;
}

// Half the size, each pixel the alpha-weighted average of a 2x2 block (an odd last row or
// column is dropped: less than a pixel of the result).
Image halve(const Image& image) {
    Image out;
    out.width = image.width / 2;
    out.height = image.height / 2;
    out.rgba.resize(std::size_t{out.width} * out.height * 4);
    const std::size_t in_stride = std::size_t{image.width} * 4;
    for (std::size_t y = 0; y < out.height; ++y) {
        const std::uint8_t* const top = &image.rgba[(y * 2) * in_stride];
        const std::uint8_t* const bottom = top + in_stride;
        std::uint8_t* const to = &out.rgba[y * std::size_t{out.width} * 4];
        for (std::size_t x = 0; x < out.width; ++x) {
            const std::array<const std::uint8_t*, 4> pixels{top + (x * 8), top + (x * 8) + 4,
                                                            bottom + (x * 8), bottom + (x * 8) + 4};
            std::uint32_t alpha = 0;
            std::array<std::uint32_t, 3> sum{};
            for (const std::uint8_t* const pixel : pixels) {
                alpha += pixel[3];
                for (std::size_t c = 0; c < 3; ++c) {
                    sum.at(c) += std::uint32_t{pixel[c]} * pixel[3];
                }
            }
            for (std::size_t c = 0; c < 3; ++c) {
                to[(x * 4) + c] =
                    static_cast<std::uint8_t>(alpha > 0 ? (sum.at(c) + (alpha / 2)) / alpha : 0);
            }
            to[(x * 4) + 3] = static_cast<std::uint8_t>((alpha + 2) / 4);
        }
    }
    return out;
}

Image area_scale(const Image& image, std::size_t out_width, std::size_t out_height) {
    const std::size_t in_width = image.width;
    const std::vector<Span> columns = spans(in_width, out_width);
    const std::vector<Span> rows = spans(image.height, out_height);
    // Photos are opaque: then there is no alpha to weight by or to divide by.
    bool opaque = true;
    for (std::size_t i = 3; i < image.rgba.size() && opaque; i += 4) {
        opaque = image.rgba[i] == 255;
    }

    // One input row as premultiplied floats (0-255), then averaged across to out_width pixels.
    std::vector<float> input(in_width * 4);
    std::vector<float> across(out_width * 4);
    std::size_t across_row = SIZE_MAX;  // the input row in across
    const auto scale_row = [&](std::size_t y) {
        const std::uint8_t* const row = &image.rgba[y * in_width * 4];
        if (opaque) {
            for (std::size_t i = 0; i < input.size(); ++i) {
                input[i] = static_cast<float>(row[i]);
            }
        } else {
            for (std::size_t x = 0; x < in_width; ++x) {
                const auto alpha = static_cast<float>(row[(x * 4) + 3]);
                for (std::size_t c = 0; c < 3; ++c) {
                    input[(x * 4) + c] = static_cast<float>(row[(x * 4) + c]) * alpha / 255.0F;
                }
                input[(x * 4) + 3] = alpha;
            }
        }
        for (std::size_t x = 0; x < out_width; ++x) {
            const Span& span = columns[x];
            const float* pixel = &input[span.first * 4];
            float r = 0.0F;
            float g = 0.0F;
            float b = 0.0F;
            float a = 0.0F;
            for (const float weight : span.weights) {
                r += weight * pixel[0];
                g += weight * pixel[1];
                b += weight * pixel[2];
                a += weight * pixel[3];
                pixel += 4;
            }
            across[(x * 4) + 0] = r;
            across[(x * 4) + 1] = g;
            across[(x * 4) + 2] = b;
            across[(x * 4) + 3] = a;
        }
        across_row = y;
    };

    // Down, one output row at a time: the input rows it covers, each scaled across once (a row
    // shared by two output rows is the last of one and the first of the next).
    Image out;
    out.width = static_cast<std::uint32_t>(out_width);
    out.height = static_cast<std::uint32_t>(out_height);
    out.rgba.resize(out_width * out_height * 4);
    std::vector<float> sum(out_width * 4);
    for (std::size_t y = 0; y < out_height; ++y) {
        const Span& span = rows[y];
        std::ranges::fill(sum, 0.0F);
        for (std::size_t k = 0; k < span.weights.size(); ++k) {
            if (across_row != span.first + k) {
                scale_row(span.first + k);
            }
            const float weight = span.weights[k];
            for (std::size_t i = 0; i < sum.size(); ++i) {
                sum[i] += weight * across[i];
            }
        }
        std::uint8_t* const to = &out.rgba[y * out_width * 4];
        if (opaque) {
            for (std::size_t i = 0; i < sum.size(); ++i) {
                to[i] = static_cast<std::uint8_t>(std::clamp(sum[i] + 0.5F, 0.0F, 255.0F));
            }
            continue;
        }
        for (std::size_t x = 0; x < out_width; ++x) {
            const float alpha = sum[(x * 4) + 3];
            const float unpremultiply = alpha > 0.0F ? 255.0F / alpha : 0.0F;
            for (std::size_t c = 0; c < 3; ++c) {
                to[(x * 4) + c] = static_cast<std::uint8_t>(
                    std::clamp((sum[(x * 4) + c] * unpremultiply) + 0.5F, 0.0F, 255.0F));
            }
            to[(x * 4) + 3] = static_cast<std::uint8_t>(std::clamp(alpha + 0.5F, 0.0F, 255.0F));
        }
    }
    return out;
}

}  // namespace

Image scale_to_fit(const Image& image, std::uint32_t max_width, std::uint32_t max_height) {
    if (image.empty() || max_width == 0 || max_height == 0 ||
        (image.width <= max_width && image.height <= max_height)) {
        return image;
    }
    const double factor = std::min(static_cast<double>(max_width) / image.width,
                                   static_cast<double>(max_height) / image.height);
    const auto out_width =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(image.width * factor)));
    const auto out_height =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(image.height * factor)));
    // Halving while the half is still at least as big as the result costs a fraction of the area
    // filter, which then does what is left.
    if (image.width >= out_width * 2 && image.height >= out_height * 2) {
        Image half = halve(image);
        while (half.width >= out_width * 2 && half.height >= out_height * 2) {
            half = halve(half);
        }
        return area_scale(half, out_width, out_height);
    }
    return area_scale(image, out_width, out_height);
}

}  // namespace rummage::picture
