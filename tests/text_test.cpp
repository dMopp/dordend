#include "rummage/text/text.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/formats/text/read.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
namespace text = rummage::text;
namespace family = rummage::formats::text;
using rummage::test::TempDir;
using Lines = std::vector<std::string>;

namespace {

text::Loaded decode(std::string_view bytes, bool more = false, const text::Limits& limits = {}) {
    return family::decode(
        std::span{reinterpret_cast<const std::uint8_t*>(bytes.data()),  // NOLINT: bytes
                  bytes.size()},
        more, limits);
}

Lines lines_of(std::string_view bytes, const text::Limits& limits = {}) {
    const text::Loaded loaded = decode(bytes, false, limits);
    CHECK(loaded.failure == text::Failure::none);
    return loaded.text.lines;
}

fs::path write(const fs::path& path, const std::string& bytes) {
    std::ofstream{path, std::ios::binary} << bytes;
    return path;
}

}  // namespace

TEST_CASE("text: split into lines, tabs expanded") {
    CHECK(lines_of("one\ntwo\r\n\nthree") == Lines{"one", "two", "", "three"});
    CHECK(lines_of("end\n") == Lines{"end"});
    CHECK(lines_of("").empty());
    CHECK(lines_of("\n") == Lines{""});
    // Tab stops every 4 columns; a character of several bytes is one column.
    CHECK(lines_of("\tx\nab\tc\nè\tz") == Lines{"    x", "ab  c", "è   z"});
    CHECK(lines_of("\tx", {.tab_width = 8}) == Lines{"        x"});
    // A carriage return alone is not a line end.
    CHECK(lines_of("a\rb\n") == Lines{"a\rb"});
}

TEST_CASE("text: CRLF is seen") {
    CHECK_FALSE(decode("a\nb\n").text.crlf);
    CHECK(decode("a\r\nb\r\n").text.crlf);
    CHECK(decode("a\nb\r\nc").text.crlf);  // mixed
    CHECK_FALSE(decode("a\r").text.crlf);  // no line end after it
}

TEST_CASE("text: UTF-8, with or without its byte order mark") {
    const text::Loaded plain = decode("caffè ☕ 😀\n");
    CHECK(plain.text.encoding == text::Encoding::utf8);
    CHECK(plain.text.lines == Lines{"caffè ☕ 😀"});
    const text::Loaded marked = decode("\xEF\xBB\xBFhi");
    CHECK(marked.text.encoding == text::Encoding::utf8);
    CHECK(marked.text.lines == Lines{"hi"});
    // A character cut at the end of the file: one that cannot be read; at the end of what was
    // read, when the file goes on: left for later (and its line with it, unless it is the only
    // one).
    CHECK(lines_of("ok \xC3") == Lines{"ok \xEF\xBF\xBD"});
    CHECK(decode("ok \xC3", true).text.lines == Lines{"ok "});
    CHECK(decode("ok\nnext \xE2\x98", true).text.lines == Lines{"ok"});
    CHECK(decode("ok \xE2\x98", true).text.encoding == text::Encoding::utf8);
}

TEST_CASE("text: not UTF-8 is Windows-1252, Latin-1's superset") {
    // "caffè costa 3€, “doppio”" written by an old Windows editor.
    const text::Loaded old = decode(
        "caff\xE8 costa 3\x80, \x93"
        "doppio\x94\n");
    CHECK(old.failure == text::Failure::none);
    CHECK(old.text.encoding == text::Encoding::cp1252);
    CHECK(old.text.lines == Lines{"caffè costa 3€, “doppio”"});
    CHECK(text::encoding_name(old.text.encoding) == "Latin-1");
    // Every byte from 0x80 to 0xFF, as the WHATWG Encoding Standard reads windows-1252 (the five
    // it leaves undefined are their C1 controls).
    std::string high;
    for (int byte = 0x80; byte <= 0xFF; ++byte) {
        high.push_back(static_cast<char>(byte));
    }
    const std::string expected =
        "€\xC2\x81‚ƒ„…†‡ˆ‰Š‹Œ\xC2\x8DŽ\xC2\x8F\xC2\x90‘’“”•–—˜™š›œ\xC2\x9DžŸ"
        "\xC2\xA0¡¢£¤¥¦§¨©ª«¬\xC2\xAD®¯°±²³´µ¶·¸¹º»¼½¾¿ÀÁÂÃÄÅÆÇÈÉÊËÌÍÎÏÐÑÒÓÔÕÖ×ØÙÚÛÜÝÞß"
        "àáâãäåæçèéêëìíîïðñòóôõö÷øùúûüýþÿ";
    CHECK(lines_of(high) == Lines{expected});
    // What UTF-8 never holds: overlong forms, surrogates, code points past U+10FFFF, a lone
    // continuation byte.
    for (const std::string_view bad : {"\xC0\xAF", "\xE0\x80\xAF", "\xED\xA0\x80",
                                       "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "a\x80z"}) {
        CAPTURE(bad);
        CHECK(decode(bad).text.encoding == text::Encoding::cp1252);
    }
}

TEST_CASE("text: UTF-16 after its byte order mark") {
    const text::Loaded little = decode(std::string_view{"\xFF\xFEh\0i\0\n\0\xE8\0", 10});
    CHECK(little.failure == text::Failure::none);
    CHECK(little.text.encoding == text::Encoding::utf16le);
    CHECK(little.text.lines == Lines{"hi", "è"});
    CHECK(text::encoding_name(little.text.encoding) == "UTF-16 LE");
    const text::Loaded big = decode(std::string_view{"\xFE\xFF\0h\0i\x20\xAC", 8});
    CHECK(big.text.encoding == text::Encoding::utf16be);
    CHECK(big.text.lines == Lines{"hi€"});
    // A pair of surrogates is one character; a lone one cannot be read.
    CHECK(lines_of(std::string_view{"\xFF\xFE\x3D\xD8\x00\xDE", 6}) == Lines{"😀"});
    CHECK(lines_of(std::string_view{"\xFF\xFE\x3D\xD8x\0", 6}) == Lines{"\xEF\xBF\xBDx"});
    CHECK(lines_of(std::string_view{"\xFF\xFE\x00\xDEx\0", 6}) == Lines{"\xEF\xBF\xBDx"});
    // Cut at the end of the file: cannot be read; at the end of what was read: left for later.
    CHECK(lines_of(std::string_view{"\xFF\xFEh\0i", 5}) == Lines{"h\xEF\xBF\xBD"});
    CHECK(decode(std::string_view{"\xFF\xFEh\0i", 5}, true).text.lines == Lines{"h"});
    CHECK(decode(std::string_view{"\xFF\xFEh\0\x3D\xD8", 6}, true).text.lines == Lines{"h"});
    CHECK(lines_of(std::string_view{"\xFF\xFEh\0\x3D\xD8", 6}) == Lines{"h\xEF\xBF\xBD"});
}

TEST_CASE("text: binary is not text") {
    CHECK(decode(std::string_view{"a\0b", 3}).failure == text::Failure::binary);
    CHECK(decode(std::string_view{"\xEF\xBB\xBF\0", 4}).failure == text::Failure::binary);
    // UTF-16 holding U+0000 (and so UTF-32).
    CHECK(decode(std::string_view{"\xFF\xFEh\0\0\0", 6}).failure == text::Failure::binary);
    CHECK(decode(std::string_view{"\xFF\xFE\0\0h\0\0\0", 8}).failure == text::Failure::binary);
    CHECK(decode(std::string_view{"\x89PNG\r\n\x1A\n\0\0\0\rIHDR", 16}).failure ==
          text::Failure::binary);
}

TEST_CASE("text: long files and long lines are cut") {
    // 10 bytes read of "aaa\nbbb\nccc\nddd": the incomplete third line is left out.
    const text::Loaded head = decode("aaa\nbbb\nc", true);
    CHECK(head.text.truncated);
    CHECK(head.text.lines == Lines{"aaa", "bbb"});
    // The only line, incomplete: shown.
    CHECK(decode("aaaa", true).text.lines == Lines{"aaaa"});
    const text::Loaded whole = decode("aaa\nbbb\nccc\nddd");
    CHECK_FALSE(whole.text.truncated);
    CHECK(whole.text.lines.size() == 4);
    // At most so many lines.
    const text::Loaded few = decode("1\n2\n3\n4\n", false, {.max_lines = 2});
    CHECK(few.text.lines == Lines{"1", "2"});
    CHECK(few.text.truncated);
    CHECK_FALSE(decode("1\n2\n", false, {.max_lines = 2}).text.truncated);
    // Lines are cut at a character boundary: "è" is two bytes.
    CHECK(lines_of("abcdef\naèèè\n\tx", {.max_line_bytes = 4}) == Lines{"abcd", "aè", "    "});
}

TEST_CASE("text: read from a file") {
    const TempDir dir;
    const fs::path file = write(dir.path / "a.txt", "aaa\nbbb\nccc\nddd");
    const text::Loaded head =
        family::load({.path = file, .mime = "text/plain", .limits = {.max_bytes = 10}});
    CHECK(head.failure == text::Failure::none);
    CHECK(head.text.truncated);
    CHECK(head.text.lines == Lines{"aaa", "bbb"});
    const text::Loaded whole =
        family::load({.path = file, .mime = {}, .limits = {.max_bytes = 15}});
    CHECK_FALSE(whole.text.truncated);
    CHECK(whole.text.lines == Lines{"aaa", "bbb", "ccc", "ddd"});

    const text::Loaded missing =
        family::load({.path = dir.path / "missing.txt", .mime = {}, .limits = {}});
    CHECK(missing.failure == text::Failure::unreadable);
    CHECK_FALSE(missing.detail.empty());
    CHECK(family::load({.path = dir.path, .mime = {}, .limits = {}}).failure ==
          text::Failure::unreadable);
}

TEST_CASE("text: a log's end, from a line's start") {
    const TempDir dir;
    const fs::path file = write(dir.path / "app.log", "aaa\nbbb\nccc\nddd\n");
    // The last 9 bytes are "\nccc\nddd\n": the cut line before them is left out.
    const text::Loaded end =
        family::load({.path = file, .mime = {}, .limits = {.max_bytes = 9, .tail = true}});
    CHECK(end.failure == text::Failure::none);
    CHECK(end.text.lines == Lines{"ccc", "ddd"});
    CHECK(end.text.tail);
    CHECK(end.text.truncated);
    // A file within the limit is read whole, from its start.
    const text::Loaded whole =
        family::load({.path = file, .mime = {}, .limits = {.max_bytes = 64, .tail = true}});
    CHECK(whole.text.lines == Lines{"aaa", "bbb", "ccc", "ddd"});
    CHECK_FALSE(whole.text.tail);
    CHECK_FALSE(whole.text.truncated);
    // One long line: its end, from a character's start (never half an "è").
    const fs::path one = write(dir.path / "one.log", "èèèèè");
    const text::Loaded cut =
        family::load({.path = one, .mime = {}, .limits = {.max_bytes = 5, .tail = true}});
    CHECK(cut.text.lines == Lines{"èè"});
    CHECK(cut.text.encoding == text::Encoding::utf8);
}

TEST_CASE("text: minified files look so") {
    text::Text text;
    CHECK_FALSE(text::looks_minified(text));
    text.lines = {std::string(1100, 'x'), "a"};
    CHECK(text::looks_minified(text));
    text.lines.emplace_back("b");
    CHECK_FALSE(text::looks_minified(text));  // 1102 bytes over three lines
}

TEST_CASE("text: no loader, no text") {
    const text::Loader kept = [](const text::LoadRequest& request) {
        return family::load(request);
    };
    text::set_loader({});
    const text::Loaded none = text::load_text({.path = "/etc/hostname", .mime = {}, .limits = {}});
    CHECK(none.failure == text::Failure::unavailable);
    CHECK_FALSE(none.detail.empty());
    text::set_loader(kept);  // what the other tests use
}
