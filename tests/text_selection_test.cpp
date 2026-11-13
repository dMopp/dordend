#include "rummage/views/text_selection.hpp"

#include <string>
#include <vector>

#include <doctest/doctest.h>

using rummage::views::byte_at_column;
using rummage::views::column_at_byte;
using rummage::views::TextPoint;
using rummage::views::TextSelection;

namespace {

const std::vector<std::string> kLines{"int main() {", "    return caffè_1;", "}"};

std::string selected(const TextSelection& selection) { return selection.text(kLines); }

}  // namespace

TEST_CASE("text selection: columns and bytes count characters") {
    CHECK(byte_at_column("abc", 0) == 0);
    CHECK(byte_at_column("abc", 2) == 2);
    CHECK(byte_at_column("abc", 9) == 3);  // past the end: the end
    // "è" is two bytes, one column.
    CHECK(byte_at_column("caffè!", 4) == 4);
    CHECK(byte_at_column("caffè!", 5) == 6);
    CHECK(column_at_byte("caffè!", 6) == 5);
    CHECK(column_at_byte("caffè!", 99) == 6);
}

TEST_CASE("text selection: a drag selects characters, over lines, either way") {
    TextSelection selection;
    CHECK(selection.empty());
    selection.press({0, 4}, 1, false, kLines);
    CHECK(selection.empty());  // a click alone selects nothing
    selection.drag({0, 8}, kLines);
    CHECK(selected(selection) == "main");
    selection.drag({1, 10}, kLines);
    CHECK(selected(selection) == "main() {\n    return");
    // Back past where it started: from there to the press.
    selection.drag({0, 0}, kLines);
    CHECK(selected(selection) == "int ");
    CHECK(selection.range().first == TextPoint{0, 0});
}

TEST_CASE("text selection: double click a word, triple click a line, dragging by them") {
    TextSelection selection;
    selection.press({1, 13}, 2, false, kLines);  // inside "caffè_1": letters, digits, _, è
    CHECK(selected(selection) == "caffè_1");
    selection.press({0, 8}, 2, false, kLines);  // "(": punctuation, one character
    CHECK(selected(selection) == "(");
    selection.press({1, 1}, 2, false, kLines);  // the indentation: its spaces
    CHECK(selected(selection) == "    ");
    selection.press({0, 0}, 2, false, kLines);
    selection.drag({1, 6}, kLines);  // by words: the whole of "return"
    CHECK(selected(selection) == "int main() {\n    return");
    selection.press({1, 3}, 3, false, kLines);
    CHECK(selected(selection) == "    return caffè_1;\n");
    selection.drag({2, 0}, kLines);  // the last line has no break after it
    CHECK(selected(selection) == "    return caffè_1;\n}");
}

TEST_CASE("text selection: Shift+click extends, select all, clear") {
    TextSelection selection;
    selection.press({0, 4}, 1, false, kLines);
    selection.press({0, 8}, 1, true, kLines);  // from the last press
    CHECK(selected(selection) == "main");
    selection.press({1, 4}, 1, true, kLines);
    CHECK(selected(selection) == "main() {\n    ");
    selection.select_all(kLines);
    CHECK(selected(selection) == "int main() {\n    return caffè_1;\n}");
    selection.clear();
    CHECK(selection.empty());
    CHECK(selected(selection).empty());
    // A point inside a character counts from its start; no text, nothing.
    selection.press({1, 16}, 1, false, kLines);  // the second byte of "è" (15-16)
    selection.drag({1, 17}, kLines);
    CHECK(selected(selection) == "è");
    selection.select_all({});
    CHECK(selection.empty());
}
