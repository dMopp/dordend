#pragma once

#include <array>
#include <cstddef>

namespace rummage::views {

// How big the rows of the lists are (the columns and the details), zoomed together with
// Ctrl+plus, Ctrl+minus, Ctrl+wheel and Ctrl+0 (ROADMAP 8.1): the row, its icon, the name and the
// details' other cells.
struct ListSize {
    float row{28.0F};
    float icon{18.0F};
    float text{14.0F};
    float detail{13.0F};

    bool operator==(const ListSize&) const = default;
};

inline constexpr std::array<ListSize, 5> kListSizes{{
    {.row = 24.0F, .icon = 16.0F, .text = 13.0F, .detail = 12.0F},
    {.row = 28.0F, .icon = 18.0F, .text = 14.0F, .detail = 13.0F},  // the usual size
    {.row = 34.0F, .icon = 22.0F, .text = 15.0F, .detail = 13.0F},
    {.row = 42.0F, .icon = 28.0F, .text = 16.0F, .detail = 14.0F},
    {.row = 52.0F, .icon = 36.0F, .text = 17.0F, .detail = 15.0F},
}};

inline constexpr std::size_t kUsualListSize = 1;

}  // namespace rummage::views
