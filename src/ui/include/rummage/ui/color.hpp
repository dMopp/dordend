#pragma once

#include <cstdint>

namespace rummage::ui {

// Straight (non-premultiplied) RGBA, components in [0, 1], sRGB-encoded like the theme colors.
struct Color {
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
    float a{1.0F};

    // 0xRRGGBBAA, the notation used by Noctalia's palettes and Umbriel's config.
    [[nodiscard]] static constexpr Color rgba(std::uint32_t hex) noexcept {
        constexpr float max_component = 255.0F;
        return {static_cast<float>((hex >> 24U) & 0xFFU) / max_component,
                static_cast<float>((hex >> 16U) & 0xFFU) / max_component,
                static_cast<float>((hex >> 8U) & 0xFFU) / max_component,
                static_cast<float>(hex & 0xFFU) / max_component};
    }

    // 0xRRGGBB, opaque.
    [[nodiscard]] static constexpr Color rgb(std::uint32_t hex) noexcept {
        return rgba((hex << 8U) | 0xFFU);
    }

    [[nodiscard]] constexpr Color with_alpha(float alpha) const noexcept {
        return {r, g, b, alpha};
    }

    [[nodiscard]] constexpr Color premultiplied() const noexcept {
        return {r * a, g * a, b * a, a};
    }

    bool operator==(const Color&) const = default;
};

}  // namespace rummage::ui
