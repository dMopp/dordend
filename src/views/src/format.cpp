#include "nordend/views/format.hpp"
#include "nordend/core/i18n.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <format>

namespace nordend::views {

std::string format_size(std::uint64_t bytes) {
    if (bytes == 1) {
        return std::string{i18n::tr("1 byte")};
    }
    constexpr std::uint64_t step = 1000;
    if (bytes < step) {
        return i18n::trf("{} bytes", bytes);
    }
    constexpr std::array<const char*, 6> units{"kB", "MB", "GB", "TB", "PB", "EB"};
    auto value = static_cast<double>(bytes) / static_cast<double>(step);
    std::size_t unit = 0;
    // Round first, so 999 950 bytes reads "1.0 MB", not "1000.0 kB".
    while (std::round(value * 10.0) / 10.0 >= static_cast<double>(step) &&
           unit + 1 < units.size()) {
        value /= static_cast<double>(step);
        ++unit;
    }
    return i18n::decimal(std::format("{:.1f} {}", value, units.at(unit)));
}

std::string format_time(std::int64_t nanoseconds_since_epoch) {
    const auto seconds = static_cast<std::time_t>(nanoseconds_since_epoch / 1'000'000'000);
    std::tm local{};
    if (localtime_r(&seconds, &local) == nullptr) {
        return {};
    }
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1,
                       local.tm_mday, local.tm_hour, local.tm_min);
}

RowRange visible_rows(std::size_t count, float row_height, float offset, float viewport) {
    if (count == 0 || row_height <= 0.0F || viewport <= 0.0F) {
        return {};
    }
    const auto first = static_cast<std::size_t>(std::max(0.0F, std::floor(offset / row_height)));
    const auto last =
        static_cast<std::size_t>(std::max(0.0F, std::ceil((offset + viewport) / row_height)));
    return {std::min(first, count), std::min(last, count)};
}

std::vector<std::size_t> rows_touched(float top, float bottom, float row_height,
                                      std::size_t count) {
    const RowRange range =
        visible_rows(count, row_height, std::max(0.0F, top), std::max(bottom - top, 0.01F));
    std::vector<std::size_t> rows;
    rows.reserve(range.size());
    for (std::size_t row = range.first; row < range.last; ++row) {
        rows.push_back(row);
    }
    return rows;
}

}  // namespace nordend::views
