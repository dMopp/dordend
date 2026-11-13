#pragma once

#include <algorithm>

namespace rummage::ui {

// Axis-aligned rectangle in logical pixels, origin at the top-left, y pointing down.
struct Rect {
    float x{0.0F};
    float y{0.0F};
    float width{0.0F};
    float height{0.0F};

    [[nodiscard]] constexpr float right() const noexcept { return x + width; }

    [[nodiscard]] constexpr float bottom() const noexcept { return y + height; }

    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0.0F || height <= 0.0F; }

    // The point is inside: left and top edges included, right and bottom left out.
    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept {
        return px >= x && px < right() && py >= y && py < bottom();
    }

    [[nodiscard]] constexpr Rect intersect(const Rect& other) const noexcept {
        const float left = std::max(x, other.x);
        const float top = std::max(y, other.y);
        const float r = std::min(right(), other.right());
        const float b = std::min(bottom(), other.bottom());
        return {left, top, std::max(0.0F, r - left), std::max(0.0F, b - top)};
    }

    // Shrinks (positive amount) or grows (negative) the rectangle on every side.
    [[nodiscard]] constexpr Rect inset(float amount) const noexcept {
        return {x + amount, y + amount, width - (2.0F * amount), height - (2.0F * amount)};
    }
};

// Corner radii in logical pixels.
struct Corners {
    float top_left{0.0F};
    float top_right{0.0F};
    float bottom_right{0.0F};
    float bottom_left{0.0F};

    [[nodiscard]] static constexpr Corners all(float radius) noexcept {
        return {radius, radius, radius, radius};
    }
};

}  // namespace rummage::ui
