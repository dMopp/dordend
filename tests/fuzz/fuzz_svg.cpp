#include <cstddef>
#include <cstdint>

#include "rummage/svg/svg.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Keep seeds as literal SVGs and exercise both tiny and larger offscreen layers.
    const int pixels = size % 16 == 0 ? 128 : size % 16 == 1 ? 1 : 16;
    static_cast<void>(rummage::svg::render({data, size}, pixels));
    return 0;
}
