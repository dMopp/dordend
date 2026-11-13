#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Selecting text on Quick Look's text page (ROADMAP 3b-3): where a selection starts and ends, by
// characters, words or lines, and the text it holds. No drawing and no pointer here: the page
// turns positions into points, this keeps what they select.
namespace rummage::views {

// A place in a text: a line and a byte in it (at a character's start).
struct TextPoint {
    std::size_t line{0};
    std::size_t byte{0};

    auto operator<=>(const TextPoint&) const = default;
};

// The byte where the character at column (counted in characters, from 0) starts; the line's
// end past its last character. A monospace page: one character, one column.
[[nodiscard]] std::size_t byte_at_column(std::string_view line, std::size_t column);

// The column of the character starting at byte (the characters before it).
[[nodiscard]] std::size_t column_at_byte(std::string_view line, std::size_t byte);

class TextSelection {
public:
    using Lines = std::span<const std::string>;

    // A press at a point: one click a character's place, two the word there, three its line;
    // extend (Shift) moves the end, the start stays.
    void press(TextPoint at, int clicks, bool extend, Lines lines);

    // The pointer moved to at with the button down: the selection reaches it, by the unit the
    // press chose (characters, words, lines), keeping what the press took.
    void drag(TextPoint at, Lines lines);

    void select_all(Lines lines);

    void clear() { *this = {}; }

    [[nodiscard]] bool empty() const noexcept { return first_ == last_; }

    // From its first point to just past its last, in order.
    [[nodiscard]] std::pair<TextPoint, TextPoint> range() const noexcept { return {first_, last_}; }

    // The selected text, lines joined by "\n".
    [[nodiscard]] std::string text(Lines lines) const;

private:
    enum class Unit : std::uint8_t { character, word, line };

    // The unit's span around at: [start, end).
    [[nodiscard]] std::pair<TextPoint, TextPoint> unit_at(TextPoint at, Lines lines) const;

    Unit unit_{Unit::character};
    // What the press took (one point for characters, a word or a line otherwise): a drag
    // keeps it and reaches out from it.
    TextPoint pressed_first_;
    TextPoint pressed_last_;
    TextPoint first_;
    TextPoint last_;
};

}  // namespace rummage::views
