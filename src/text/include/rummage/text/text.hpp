#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// What rummage does with a text file, wherever it is read (ROADMAP 3b): the Text it draws and
// how one is asked for. No reader here: the text family (formats/text) reads files in the base
// engine, and rummage asks it through the loader, as for pictures.
namespace rummage::text {

// How the file's bytes were read. Every Text holds UTF-8 whatever the file's encoding.
enum class Encoding : std::uint8_t {
    utf8,     // valid UTF-8 (plain ASCII included), with or without a byte order mark
    utf16le,  // after its byte order mark
    utf16be,
    cp1252,  // not UTF-8: Windows-1252, the superset of Latin-1 browsers read "Latin-1" as
};

// Its name for people: "UTF-8", "UTF-16 LE", "UTF-16 BE", "Latin-1".
[[nodiscard]] std::string_view encoding_name(Encoding encoding) noexcept;

// What a piece of a line is, for its colour (ROADMAP 3b-2). Plain text has none.
enum class Style : std::uint8_t {
    keyword,   // if, return, fn, def
    type,      // int, std::string, Vec, a CamelCase name
    string,    // "text", 'c', R"(raw)", <header.h> after #include
    number,    // 42, 0x1F, 1.5e3, and constants: true, nullptr, None
    comment,   // // and /* */
    function,  // a name called, a macro, #include, @decorator, #[attribute]
    added,     // a diff's + lines
    removed,   // a diff's - lines
};
inline constexpr std::size_t kStyleCount = 8;

// A coloured piece of a line: its bytes [start, start + length).
struct Span {
    std::uint32_t start{0};
    std::uint32_t length{0};
    Style style{Style::keyword};

    bool operator==(const Span&) const = default;
};

// The beginning of a text file (or its end, for a log), ready to draw line by line.
struct Text {
    std::vector<std::string> lines;  // UTF-8, tabs expanded, no line ends, never a '\n'
    bool truncated{false};           // the file goes on past what was read
    bool tail{false};                // the lines are the file's end: what came before was left out
    Encoding encoding{Encoding::utf8};
    bool crlf{false};  // some line ended with "\r\n"
    // The language the lines were coloured as ("C++"); empty, and no spans, when none.
    std::string language;
    // Each line's coloured pieces, in order, apart; empty (not one per line) without a language.
    std::vector<std::vector<Span>> spans;
};

struct Limits {
    std::size_t max_bytes{std::size_t{256} * 1024};  // read at most this much of the file
    std::size_t max_line_bytes{1024};                // longer lines are cut (minified files)
    std::size_t max_lines{0};                        // more are left out; 0: no limit
    std::size_t tab_width{4};
    // Read the file's end (a log's newest lines): its last max_bytes, from a line's start.
    bool tail{false};
};

// Lines this long on average are a minified file's (JavaScript, CSS, JSON on one line): shown
// without colours, which would only paint a wall.
inline constexpr std::size_t kMinifiedLineBytes = 500;

// Whether text's lines average more than kMinifiedLineBytes.
[[nodiscard]] bool looks_minified(const Text& text) noexcept;

enum class Failure : std::uint8_t {
    none,
    binary,       // a NUL byte (or character): not text
    unreadable,   // the file could not be read
    unavailable,  // no engine could read it (none installed, or it failed)
};

struct LoadRequest {
    std::filesystem::path path;
    std::string mime;  // the type rummage found, for the engine to choose a language by
    Limits limits;
};

struct Loaded {
    Failure failure{Failure::none};
    std::string detail;  // why, for the log, when it failed
    Text text;
};

// Where texts are read: rummage sets its engine's helpers, tests and tools the text family in
// their own process. Set before use.
using Loader = std::function<Loaded(const LoadRequest&)>;
void set_loader(Loader loader);

// The text of request.path through the loader; unavailable when none is set. Thread-safe.
[[nodiscard]] Loaded load_text(const LoadRequest& request);

}  // namespace rummage::text
