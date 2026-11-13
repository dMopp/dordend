#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rummage::views {

// "1 byte", "512 bytes", "1.2 kB", "45.0 MB", "1.3 GB": decimal (SI) units, as GNOME shows sizes.
[[nodiscard]] std::string format_size(std::uint64_t bytes);

// Local date and time, "2026-10-26 14:03": unambiguous whatever the reader's habits.
[[nodiscard]] std::string format_time(std::int64_t nanoseconds_since_epoch);

// Rows [first, last) of a list of `count` rows of `row_height` that intersect a viewport of
// `viewport` pixels scrolled by `offset`: the only rows worth drawing.
struct RowRange {
    std::size_t first{0};
    std::size_t last{0};

    [[nodiscard]] std::size_t size() const noexcept { return last - first; }
};

[[nodiscard]] RowRange visible_rows(std::size_t count, float row_height, float offset,
                                    float viewport);

// The rows of such a list that a band from y `top` to `bottom` (content pixels) touches; a flat
// band touches the row it lies on.
[[nodiscard]] std::vector<std::size_t> rows_touched(float top, float bottom, float row_height,
                                                    std::size_t count);

}  // namespace rummage::views
