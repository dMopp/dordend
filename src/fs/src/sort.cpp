#include "rummage/fs/sort.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace rummage::fs {
namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

char fold(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

int sign(int value) noexcept { return static_cast<int>(value > 0) - static_cast<int>(value < 0); }

template <class T>
int three_way(T a, T b) noexcept {
    return static_cast<int>(a > b) - static_cast<int>(a < b);
}

}  // namespace

int natural_compare(std::string_view a, std::string_view b) noexcept {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            // Compare digit runs by value: skip leading zeros, then the longer run is larger,
            // then the first differing digit decides.
            std::size_t a_start = i;
            std::size_t b_start = j;
            while (a_start < a.size() && a[a_start] == '0') {
                ++a_start;
            }
            while (b_start < b.size() && b[b_start] == '0') {
                ++b_start;
            }
            std::size_t a_end = a_start;
            std::size_t b_end = b_start;
            while (a_end < a.size() && is_digit(a[a_end])) {
                ++a_end;
            }
            while (b_end < b.size() && is_digit(b[b_end])) {
                ++b_end;
            }
            if (const std::size_t a_len = a_end - a_start, b_len = b_end - b_start;
                a_len != b_len) {
                return a_len < b_len ? -1 : 1;
            }
            if (const int digits =
                    a.substr(a_start, a_end - a_start).compare(b.substr(b_start, b_end - b_start));
                digits != 0) {
                return sign(digits);
            }
            i = a_end;  // equal value; leading zeros are settled by the final tie-break
            j = b_end;
            continue;
        }
        const auto ca = static_cast<unsigned char>(fold(a[i]));
        const auto cb = static_cast<unsigned char>(fold(b[j]));
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        ++i;
        ++j;
    }
    if (i < a.size() || j < b.size()) {
        return i < a.size() ? 1 : -1;  // the one with characters left is longer
    }
    return sign(a.compare(b));
}

bool SortOrder::operator()(const Entry& a, const Entry& b) const noexcept {
    if (directories_first && a.is_directory() != b.is_directory()) {
        return a.is_directory();  // not reversed by descending
    }
    int result = 0;
    switch (key) {
        case SortKey::size:
            // A directory's size is its listing's, not its content's: directories keep their
            // name order among themselves.
            if (!a.is_directory() || !b.is_directory()) {
                result = three_way(a.size, b.size);
            }
            break;
        case SortKey::modified:
            result = three_way(a.modified_ns, b.modified_ns);
            break;
        case SortKey::type: {
            // A name that gives several types sorts by its best one (Entry::name_guess).
            const MimeId ta = a.mime != kUnknownMime ? a.mime : a.name_guess;
            const MimeId tb = b.mime != kUnknownMime ? b.mime : b.name_guess;
            if ((ta == kUnknownMime) != (tb == kUnknownMime)) {
                result = ta == kUnknownMime ? 1 : -1;
            } else if (mime != nullptr) {
                result = sign(mime->name(ta).compare(mime->name(tb)));
            } else {
                result = three_way(ta, tb);
            }
            break;
        }
        case SortKey::name:
            break;
    }
    if (result == 0) {
        result = natural_compare(a.name, b.name);
    }
    return descending ? result > 0 : result < 0;
}

void Listing::merge(std::vector<Entry> chunk) {
    std::ranges::sort(chunk, order_);
    const auto middle = static_cast<std::ptrdiff_t>(entries_.size());
    entries_.insert(entries_.end(), std::make_move_iterator(chunk.begin()),
                    std::make_move_iterator(chunk.end()));
    std::ranges::inplace_merge(entries_, entries_.begin() + middle, order_);
}

void Listing::add(std::vector<Entry> chunk) {
    if (!show_hidden_) {
        const auto out = std::ranges::stable_partition(
            chunk, [this](const Entry& entry) { return !left_out(entry); });
        hidden_.insert(hidden_.end(), std::make_move_iterator(out.begin()),
                       std::make_move_iterator(out.end()));
        chunk.erase(out.begin(), out.end());
    }
    merge(std::move(chunk));
}

void Listing::replace(const std::unordered_set<std::string>& remove, std::vector<Entry> insert) {
    std::unordered_set<std::string_view> replaced;
    replaced.reserve(insert.size());
    for (const Entry& entry : insert) {
        replaced.insert(entry.name);
    }
    const auto gone = [&](const Entry& entry) {
        return remove.contains(entry.name) || replaced.contains(entry.name);
    };
    std::erase_if(entries_, gone);
    std::erase_if(hidden_, gone);
    add(std::move(insert));
}

void Listing::set_order(SortOrder order) {
    order_ = order;
    std::ranges::sort(entries_, order_);
}

Listing::Moved Listing::set_filter(bool show_hidden, std::string keep) {
    show_hidden_ = show_hidden;
    keep_ = std::move(keep);
    Moved moved;
    const auto back =
        std::ranges::partition(hidden_, [this](const Entry& entry) { return left_out(entry); });
    std::vector<Entry> shown(std::make_move_iterator(back.begin()),
                             std::make_move_iterator(back.end()));
    hidden_.erase(back.begin(), back.end());
    const auto out = std::ranges::stable_partition(
        entries_, [this](const Entry& entry) { return !left_out(entry); });
    for (Entry& entry : out) {
        moved.hidden.push_back(entry.name);
        hidden_.push_back(std::move(entry));
    }
    entries_.erase(out.begin(), out.end());
    for (const Entry& entry : shown) {
        moved.shown.push_back(entry.name);
    }
    merge(std::move(shown));
    return moved;
}

}  // namespace rummage::fs
