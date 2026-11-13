#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rummage::views {

// Logical pixels, shared by grid layout, zoom controls and saved-state validation.
inline constexpr std::array<std::int32_t, 8> kGridIconSizes{48, 64, 96, 128, 192, 256, 384, 512};
inline constexpr std::size_t kUsualGridSize = 2;

}  // namespace rummage::views
