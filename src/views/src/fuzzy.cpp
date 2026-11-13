#include "rummage/views/fuzzy.hpp"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "fzy/src/match.h"  // NOLINT(misc-include-cleaner): has_match, match, match_positions

namespace rummage::views {
namespace {

// A match in a path's name ranks above any match that needs its folders.
constexpr double kNameBonus = 1e10;

// fzy gives +infinity to a whole-text match and -infinity when it cannot tell: kept finite so
// they still sort.
double finite(double score) {
    if (std::isinf(score)) {
        return score > 0.0 ? 1e9 : -1e9;
    }
    return score;
}

}  // namespace

FuzzyQuery::FuzzyQuery(std::string_view query) {
    // fzy widens only lowercase query characters to either case: an uppercase one would miss a
    // lowercase letter. The text it lowercases itself.
    for (const char c : query) {
        if (c != ' ' && c != '\0') {
            needle_ += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
}

bool FuzzyQuery::prepare(std::string_view text) {
    if (needle_.size() > text.size() || text.size() > kFuzzyMaxLength ||
        text.find('\0') != std::string_view::npos) {
        return false;
    }
    haystack_.assign(text);
    return has_match(needle_.c_str(), haystack_.c_str()) != 0;
}

std::optional<double> FuzzyQuery::score(std::string_view text) {
    if (needle_.empty()) {
        return 0.0;
    }
    if (!prepare(text)) {
        return std::nullopt;
    }
    return finite(match(needle_.c_str(), haystack_.c_str()));
}

std::vector<std::size_t> FuzzyQuery::positions(std::string_view text) {
    if (needle_.empty() || !prepare(text)) {
        return {};
    }
    std::vector<std::size_t> out(needle_.size());
    match_positions(needle_.c_str(), haystack_.c_str(), out.data());
    return out;
}

std::optional<double> FuzzyQuery::path_score(std::string_view path) {
    if (needle_.empty()) {
        return 0.0;
    }
    const std::size_t slash = path.rfind('/');
    if (slash != std::string_view::npos) {
        if (const std::optional<double> name = score(path.substr(slash + 1))) {
            return kNameBonus + *name;
        }
        return score(path);
    }
    if (const std::optional<double> name = score(path)) {
        return kNameBonus + *name;
    }
    return std::nullopt;
}

std::vector<std::size_t> FuzzyQuery::path_positions(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    if (slash != std::string_view::npos) {
        std::vector<std::size_t> in_name = positions(path.substr(slash + 1));
        if (!in_name.empty()) {
            for (std::size_t& at : in_name) {
                at += slash + 1;
            }
            return in_name;
        }
    }
    return positions(path);
}

std::optional<double> fuzzy_score(std::string_view query, std::string_view text) {
    return FuzzyQuery{query}.score(text);
}

std::vector<std::size_t> fuzzy_positions(std::string_view query, std::string_view text) {
    return FuzzyQuery{query}.positions(text);
}

}  // namespace rummage::views
