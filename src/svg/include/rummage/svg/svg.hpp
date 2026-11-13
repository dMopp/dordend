#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "rummage/picture/image.hpp"

namespace rummage::svg {
struct RenderOptions {
    // At most one warning for unsupported filter primitives per document.
    void (*warning)(void*, std::string_view) = nullptr;
    void* context = nullptr;
};

// Fit the longest side to pixels, retaining aspect ratio. Straight-alpha RGBA.
// Malformed input, exhausted limits and allocation failures return an empty
// image.
[[nodiscard]] picture::Image render(std::span<const std::uint8_t> data, int pixels,
                                    RenderOptions options = {}) noexcept;
}  // namespace rummage::svg
