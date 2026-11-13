#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "rummage/formats/text/syntax.hpp"
#include "rummage/text/text.hpp"

// The languages the text family colours, and how each one's code is written (private to the
// text family: the lexer in syntax.cpp reads these tables).
namespace rummage::formats::text {

// $variables, as each language writes them.
enum class Variables : std::uint8_t {
    none,
    shell,  // $name, ${name}, $1, $@, $#, $?
    make,   // $(NAME), ${NAME}, $@, $<, $^; $(function args): the function's name
    cmake,  // ${NAME} (nested), $ENV{NAME}, $CACHE{NAME}; $<generator:expressions>
};

// Strings whose end is a delimiter of the string's own choosing, without escapes.
enum class RawStrings : std::uint8_t {
    none,
    cpp,   // R"delimiter( ... )delimiter", with u8, u, U, L before the R
    rust,  // r"...", r#"..."#, with b before the r
};

// Keys in data and configuration files, coloured as keywords.
enum class Keys : std::uint8_t {
    none,
    json,  // "key": and JSON5's key: (a string or a name before a colon)
    toml,  // key =, a."b.c".d =, at a line's start and inside { }; [table] and [[array]] headers
    ini,   // key= and key: at a line's start, Name[it]= too; [section] lines; ; and # comments
           // on their own lines only
};

// Languages with a lexer of their own instead of the tables.
enum class Dedicated : std::uint8_t {
    none,
    yaml,
    ignore,      // .gitignore and its kin: # comments, ! negations, wildcards, / anchors
    attributes,  // .gitattributes: a pattern as in .gitignore, then attributes (text, -diff,
                 // eol=lf)
    markdown,    // highlighted, not rendered; fenced code in its own language, front matter
    diff,        // unified diffs and git's patches: hunks counted, so "--- x" in one is a line
};

// The tables in languages.cpp name only what they use: GCC's -Wmissing-field-initializers
// wants every member without an initializer given one, so each has one, which clang-tidy finds
// redundant.
// NOLINTBEGIN(readability-redundant-member-init)

// How a language's code is written, for the lexer driven by tables.
struct Syntax {
    std::span<const std::string_view> line_comments{};  // to the end of the line: "//", "#", "--"
    std::string_view block_open{};                      // "/*"; empty: none
    std::string_view block_close{};                     // "*/"
    bool nested_comments{false};                        // /* /* */ */ is one comment (Rust)
    // Long brackets with any number of "=": after long_comment ("--" in Lua, "#" in CMake) a
    // comment --[==[ ... ]==]; with long_strings, [[ ... ]] a string.
    std::string_view long_comment{};
    bool long_strings{false};
    bool comment_at_word_start{false};    // # starts a comment only where a word could (shell)
    std::string_view quotes{};            // characters that open a string
    std::string_view multiline_quotes{};  // of those, the ones whose strings go on over lines
    std::string_view literal_quotes{};    // of those, the ones without escapes ('...' in TOML)
    bool triple_quotes{false};            // """ and ''' strings, over lines (Python)
    // Words glued before a quote that belong to the string: u8"", b"", f"" (compared in lower
    // case when prefix_any_case); with an r among their letters (raw_letter), no escapes.
    std::span<const std::string_view> string_prefixes{};
    bool prefix_any_case{false};
    char raw_letter{'\0'};
    RawStrings raw_strings{RawStrings::none};
    bool preprocessor{false};      // a line starting with # is a directive (C)
    bool digit_separators{false};  // 1'000'000 (C++14)
    bool dates{false};             // 1979-05-27T07:32:00-08:00 is a number (TOML)
    char decorator{'\0'};          // @name: a decorator (Python, TypeScript)
    bool rust_attributes{false};   // #[attribute] and #![attribute]
    bool bang_macros{false};       // name! is a macro (Rust)
    bool lifetimes{false};         // 'a is a lifetime, 'a' a character (Rust)
    Variables variables{Variables::none};
    bool heredocs{false};      // <<EOF ... EOF, <<-'EOF': the lines in between a string (shell)
    bool make_targets{false};  // target: at a line's start a function, NAME = a variable
    Keys keys{Keys::none};
    std::span<const std::string_view> keywords{};
    std::span<const std::string_view> types{};
    std::span<const std::string_view> constants{};
    std::span<const std::string_view> functions{};   // builtins: echo, printf, cd
    bool camel_case_types{false};                    // Registry, TextLayout: types
    bool suffix_t_types{false};                      // size_t, uint32_t: types
    std::optional<rummage::text::Style> capitals{};  // RUMMAGE_VERSION: a macro (C), a constant
    Dedicated dedicated{Dedicated::none};
};

struct Language {
    std::string_view name{};
    std::span<const std::string_view> mime{};
    std::span<const std::string_view> extensions{};    // ".rs": when the type says nothing better
    std::span<const std::string_view> file_names{};    // whole names: "Makefile"
    std::span<const std::string_view> interpreters{};  // after #!: "python3", "env node"
    std::span<const std::string_view> fence_names{};   // after ``` in Markdown: "cpp", "py"
    const Syntax* syntax{};
};

// NOLINTEND(readability-redundant-member-init)

[[nodiscard]] std::span<const Language> all_languages();

// The language a Markdown code fence names (```rust, ```Python, ```{.sh}); nullptr when none.
[[nodiscard]] const Language* language_for_fence(std::string_view name);

}  // namespace rummage::formats::text
