#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Fuzzy matching for the command palette and finding files (ROADMAP 8a, 8b), on fzy's algorithm
// (third_party/fzy): the query's characters in order anywhere in the text, scored higher when
// they are together, at the start of a word or after a slash. Case does not matter (ASCII) and
// spaces in the query are left out ("monte rosa" finds "Monte Rosa", and "MonteRosa" too); bytes
// are compared as they are, so any UTF-8 text works, accented letters matching only themselves.
namespace rummage::views {

// fzy's limit: longer texts (or queries) never match.
inline constexpr std::size_t kFuzzyMaxLength = 1024;

// A query prepared once for many texts. Not thread-safe: one per thread.
class FuzzyQuery {
public:
    explicit FuzzyQuery(std::string_view query);

    // Nothing to match: every text matches with 0.
    [[nodiscard]] bool empty() const noexcept { return needle_.empty(); }

    // The score of text, higher is better; nullopt when it does not match.
    [[nodiscard]] std::optional<double> score(std::string_view text);

    // The byte offsets in text of the query's characters as matched, ascending; empty when it does
    // not match or the query is empty.
    [[nodiscard]] std::vector<std::size_t> positions(std::string_view text);

    // For a relative path: a match in its last part (the name) always ranks above one that needs
    // the folders, then by fzy's score; nullopt when the whole path does not match.
    [[nodiscard]] std::optional<double> path_score(std::string_view path);

    // The positions shown for a path: in its name when the name matches, else across the path.
    [[nodiscard]] std::vector<std::size_t> path_positions(std::string_view path);

private:
    // text with a NUL after it, for fzy; false when it cannot be matched at all.
    bool prepare(std::string_view text);

    std::string needle_;
    std::string haystack_;
};

// One-off forms of FuzzyQuery::score and positions.
[[nodiscard]] std::optional<double> fuzzy_score(std::string_view query, std::string_view text);
[[nodiscard]] std::vector<std::size_t> fuzzy_positions(std::string_view query,
                                                       std::string_view text);

}  // namespace rummage::views
