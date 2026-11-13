#include "rummage/views/text_selection.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace rummage::views {
namespace {

// A UTF-8 continuation byte: inside a character, never at its start.
bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U; }

// Words are letters, digits, _ and anything beyond ASCII (accented letters, other scripts).
bool word_byte(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_' || byte >= 0x80U;
}

bool space_byte(char c) { return c == ' ' || c == '\t'; }

// The start of the character holding byte at.
std::size_t character_start(std::string_view line, std::size_t at) {
    at = std::min(at, line.size());
    while (at > 0 && at < line.size() && continuation(line[at])) {
        --at;
    }
    return at;
}

// Just past the character starting at at.
std::size_t character_end(std::string_view line, std::size_t at) {
    if (at >= line.size()) {
        return line.size();
    }
    ++at;
    while (at < line.size() && continuation(line[at])) {
        ++at;
    }
    return at;
}

}  // namespace

std::size_t byte_at_column(std::string_view line, std::size_t column) {
    std::size_t byte = 0;
    for (std::size_t i = 0; i < column && byte < line.size(); ++i) {
        byte = character_end(line, byte);
    }
    return byte;
}

std::size_t column_at_byte(std::string_view line, std::size_t byte) {
    byte = std::min(byte, line.size());
    std::size_t column = 0;
    for (std::size_t i = 0; i < byte; ++i) {
        column += continuation(line[i]) ? std::size_t{0} : std::size_t{1};
    }
    return column;
}

std::pair<TextPoint, TextPoint> TextSelection::unit_at(TextPoint at, Lines lines) const {
    if (lines.empty()) {
        return {};
    }
    at.line = std::min(at.line, lines.size() - 1);
    const std::string_view line = lines[at.line];
    at.byte = character_start(line, at.byte);
    switch (unit_) {
        case Unit::character:
            return {at, at};
        case Unit::line: {
            // The line with its break, so that copying lines gives lines.
            const bool last = at.line + 1 >= lines.size();
            return {{at.line, 0},
                    last ? TextPoint{at.line, line.size()} : TextPoint{at.line + 1, 0}};
        }
        case Unit::word:
            break;
    }
    if (line.empty()) {
        return {at, at};
    }
    // Past the end: the last character's word.
    std::size_t start = at.byte >= line.size() ? character_start(line, line.size() - 1) : at.byte;
    const char kind = line[start];
    const auto same = [&](char c) {
        if (word_byte(kind)) {
            return word_byte(c);
        }
        if (space_byte(kind)) {
            return space_byte(c);
        }
        return false;  // punctuation: one character at a time
    };
    std::size_t end = character_end(line, start);
    if (word_byte(kind) || space_byte(kind)) {
        while (start > 0 && same(line[start - 1])) {
            --start;
        }
        while (end < line.size() && same(line[end])) {
            ++end;
        }
    }
    return {{at.line, start}, {at.line, end}};
}

void TextSelection::press(TextPoint at, int clicks, bool extend, Lines lines) {
    if (extend && !empty()) {
        drag(at, lines);
        return;
    }
    if (extend) {
        // Shift+click with nothing selected: from where the last press was.
        const TextPoint from = pressed_first_;
        unit_ = Unit::character;
        pressed_first_ = from;
        pressed_last_ = from;
        drag(at, lines);
        return;
    }
    unit_ = Unit::character;
    if (clicks >= 3) {
        unit_ = Unit::line;
    } else if (clicks == 2) {
        unit_ = Unit::word;
    }
    const auto [first, last] = unit_at(at, lines);
    pressed_first_ = first;
    pressed_last_ = last;
    first_ = first;
    last_ = last;
}

void TextSelection::drag(TextPoint at, Lines lines) {
    const auto [first, last] = unit_at(at, lines);
    if (first < pressed_first_) {
        first_ = first;
        last_ = pressed_last_;
    } else {
        first_ = pressed_first_;
        last_ = std::max(last, pressed_last_);
    }
}

void TextSelection::select_all(Lines lines) {
    unit_ = Unit::character;
    pressed_first_ = {};
    pressed_last_ = {};
    first_ = {};
    last_ = lines.empty() ? TextPoint{} : TextPoint{lines.size() - 1, lines.back().size()};
}

std::string TextSelection::text(Lines lines) const {
    std::string out;
    if (empty() || lines.empty()) {
        return out;
    }
    const std::size_t end_line = std::min(last_.line, lines.size() - 1);
    for (std::size_t i = first_.line; i <= end_line; ++i) {
        const std::string_view line = lines[i];
        const std::size_t from = i == first_.line ? std::min(first_.byte, line.size()) : 0;
        const std::size_t to = i == last_.line ? std::min(last_.byte, line.size()) : line.size();
        if (to > from) {
            out += line.substr(from, to - from);
        }
        if (i < last_.line) {
            out += '\n';
        }
    }
    return out;
}

}  // namespace rummage::views
