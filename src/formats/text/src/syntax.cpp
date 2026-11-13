#include "rummage/formats/text/syntax.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "languages.hpp"
#include "rummage/text/text.hpp"

namespace rummage::formats::text {
namespace {

using rummage::text::Span;
using rummage::text::Style;

// Where a line ends inside something that goes on to the next.
struct State {
    enum class In : std::uint8_t { code, comment, string, heredoc, block, embedded };
    In in{In::code};
    std::string close;      // what ends it: "*/", ")delim\"", "\"\"\"", "]==]", a heredoc's word
    bool escapes{false};    // a string: a backslash escapes the next character
    int depth{0};           // nested comments: how many are open
    bool tabs{false};       // a heredoc's last line may start with tabs (<<-)
    std::size_t indent{0};  // a YAML block scalar: its lines are indented more than this
    // Markdown: inside a fenced block (```, ~~~: fence, fence_length) or the front matter
    // (fence '\0', close "---" or "+++"), its lines lexed as embedded with their own state
    // (nullptr: a plain block, a string); after_text: the line before was a paragraph's, so
    // === and --- under it underline a heading.
    const Language* embedded{nullptr};
    std::unique_ptr<State> inner;
    char fence{'\0'};
    std::size_t fence_length{0};
    bool after_text{false};
    // A diff's hunk: the lines of the old file and the new one still to come.
    std::size_t old_lines{0};
    std::size_t new_lines{0};
    // A patch mail (From <hash> first): its message, up to the first header or hunk, holds
    // no changes, so its - and + lines are a list's.
    bool mail{false};
    bool diff_seen{false};
    // A heredoc opened on this line: its lines start with the next one.
    std::string heredoc;
    bool heredoc_tabs{false};
};

// The state inside a comment or a string that may go on over lines.
State inside(State::In in, std::string close, bool escapes, int depth) {
    State state;
    state.in = in;
    state.close = std::move(close);
    state.escapes = escapes;
    state.depth = depth;
    return state;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

bool is_word_start(char c) {
    return is_upper(c) || is_lower(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80U;
}

bool is_word(char c) { return is_word_start(c) || is_digit(c); }

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; }

char lower(char c) { return is_upper(c) ? static_cast<char>(c - 'A' + 'a') : c; }

// What the lexer looks up, made once for each syntax.
struct Tables {
    std::array<bool, 256> opens{};   // may start a comment, a string, a number or a word
    std::array<bool, 256> quotes{};  // opens a string
    std::unordered_map<std::string_view, Style> words;  // keywords, constants and types
};

Tables make_tables(const Syntax& syntax) {
    Tables tables;
    const auto open = [&](char c) { tables.opens.at(static_cast<unsigned char>(c)) = true; };
    for (const std::string_view marker : syntax.line_comments) {
        open(marker.front());
    }
    if (!syntax.block_open.empty()) {
        open(syntax.block_open.front());
    }
    for (const char quote : syntax.quotes) {
        open(quote);
        tables.quotes.at(static_cast<unsigned char>(quote)) = true;
    }
    if (!syntax.long_comment.empty()) {
        open(syntax.long_comment.front());
    }
    if (syntax.long_strings) {
        open('[');
    }
    if (syntax.variables != Variables::none) {
        open('$');
    }
    if (syntax.heredocs) {
        open('<');
    }
    if (syntax.decorator != '\0') {
        open(syntax.decorator);
    }
    if (syntax.rust_attributes) {
        open('#');
    }
    for (int c = 0; c < 256; ++c) {
        const auto byte = static_cast<char>(c);
        if (is_digit(byte) || is_word_start(byte) || byte == '.') {
            open(byte);
        }
    }
    // Later lists win: a keyword over a constant over a builtin over a type.
    for (const auto& [list, style] :
         {std::pair{syntax.types, Style::type}, std::pair{syntax.functions, Style::function},
          std::pair{syntax.constants, Style::number}, std::pair{syntax.keywords, Style::keyword}}) {
        for (const std::string_view word : list) {
            tables.words[word] = style;
        }
    }
    return tables;
}

// Every syntax's tables, made the first time any is needed.
const Tables& tables_of(const Syntax& syntax) {
    static const std::unordered_map<const Syntax*, Tables> kTables = [] {
        std::unordered_map<const Syntax*, Tables> all;
        for (const Language& language : all_languages()) {
            if (!all.contains(language.syntax)) {
                all.emplace(language.syntax, make_tables(*language.syntax));
            }
        }
        return all;
    }();
    return kTables.at(&syntax);
}

// The length of a Lua long bracket at the start of text: "[[" 2, "[==[" 4; 0 when none.
std::size_t long_bracket(std::string_view text) {
    if (!text.starts_with('[')) {
        return 0;
    }
    std::size_t i = 1;
    while (i < text.size() && text[i] == '=') {
        ++i;
    }
    return i < text.size() && text[i] == '[' ? i + 1 : 0;
}

class LineLexer {
public:
    LineLexer(const Syntax& syntax, const Tables& tables, std::string_view line, State& state,
              std::vector<Span>& out, bool first = false)
        : syntax_{syntax}, tables_{tables}, line_{line}, state_{state}, out_{out}, first_{first} {}

    // NOLINTNEXTLINE(misc-no-recursion): one level: a fence never holds Markdown
    void run() {
        if (syntax_.dedicated == Dedicated::yaml) {
            yaml();
            return;
        }
        if (syntax_.dedicated == Dedicated::markdown) {
            markdown();
            return;
        }
        if (syntax_.dedicated == Dedicated::diff) {
            diff();
            return;
        }
        if (syntax_.dedicated == Dedicated::ignore || syntax_.dedicated == Dedicated::attributes) {
            ignore_line(syntax_.dedicated == Dedicated::attributes);
            return;
        }
        if (state_.in == State::In::heredoc) {
            heredoc_line();
            return;
        }
        if (state_.in == State::In::comment) {
            go_on_comment(0);
        } else if (state_.in == State::In::string) {
            go_on_string(0);
        }
        if (state_.in == State::In::code && pos_ == 0 && syntax_.preprocessor) {
            directive();
        }
        if (state_.in == State::In::code && pos_ == 0 && syntax_.make_targets) {
            make_line();
        }
        if (state_.in == State::In::code && pos_ == 0 && syntax_.keys == Keys::ini) {
            ini_line();
        }
        if (state_.in == State::In::code && pos_ == 0 && syntax_.keys == Keys::toml) {
            toml_header();
        }
        while (pos_ < line_.size() && state_.in == State::In::code) {
            step();
        }
        if (state_.in == State::In::code && !state_.heredoc.empty()) {
            state_.in = State::In::heredoc;
            state_.close = std::move(state_.heredoc);
            state_.tabs = state_.heredoc_tabs;
            state_.heredoc.clear();
        }
    }

private:
    void emit(std::size_t start, std::size_t end, Style style) {
        if (end > start) {
            out_.push_back({.start = static_cast<std::uint32_t>(start),
                            .length = static_cast<std::uint32_t>(end - start),
                            .style = style});
        }
    }

    [[nodiscard]] bool at(std::string_view text) const {
        return !text.empty() && line_.substr(pos_).starts_with(text);
    }

    [[nodiscard]] char next(std::size_t ahead = 1) const {
        return pos_ + ahead < line_.size() ? line_[pos_ + ahead] : '\0';
    }

    // A comment from start, its opening already passed, to state_.close (counting nested
    // openings when the syntax nests them); to the end of the line when it goes on.
    void go_on_comment(std::size_t start) {
        if (!syntax_.nested_comments) {
            const std::size_t end = line_.find(state_.close, pos_);
            if (end != std::string_view::npos) {
                pos_ = end + state_.close.size();
                emit(start, pos_, Style::comment);
                state_ = {};
                return;
            }
            pos_ = line_.size();
        }
        while (pos_ < line_.size()) {
            if (at(syntax_.block_open)) {
                ++state_.depth;
                pos_ += syntax_.block_open.size();
            } else if (at(state_.close)) {
                pos_ += state_.close.size();
                if (--state_.depth <= 0) {
                    emit(start, pos_, Style::comment);
                    state_ = {};
                    return;
                }
            } else {
                ++pos_;
            }
        }
        emit(start, line_.size(), Style::comment);
        state_.in = State::In::comment;
    }

    void open_comment(std::size_t start, std::size_t opening, std::string close) {
        pos_ = start + opening;
        state_ = inside(State::In::comment, std::move(close), false, 1);
        go_on_comment(start);
    }

    // A string from start, its opening already passed, to state_.close; to the end of the line
    // when it goes on (for strings that may: otherwise it ends there).
    void go_on_string(std::size_t start) {
        const std::string_view close = state_.close;
        std::size_t end = std::string_view::npos;
        if (!state_.escapes) {
            end = line_.find(close, pos_);
        } else {
            // Most strings close with one quote: compare characters, not substrings.
            for (std::size_t i = pos_; i < line_.size(); ++i) {
                if (line_[i] == '\\') {
                    ++i;
                } else if (line_[i] == close.front() && line_.substr(i).starts_with(close)) {
                    end = i;
                    break;
                }
            }
        }
        if (end == std::string_view::npos) {
            pos_ = line_.size();
            emit(start, pos_, Style::string);
            return;
        }
        pos_ = end + close.size();
        emit(start, pos_, Style::string);
        state_ = {};
    }

    // A string opened at start, the text before its quote at quote a prefix; over lines when
    // multiline (a backslash at the end of a line goes on too, in any string with escapes).
    void open_string(std::size_t start, std::size_t quote, std::string close, bool escapes,
                     bool multiline) {
        pos_ = quote + close.size();
        state_ = inside(State::In::string, std::move(close), escapes, 0);
        go_on_string(start);
        if (state_.in == State::In::string && !multiline && !(escapes && line_.ends_with('\\'))) {
            state_ = {};  // a string never closed: it ends with its line
        }
    }

    // # include <file>: the directive as a macro, an included file as a string.
    void directive() {
        std::size_t i = 0;
        while (i < line_.size() && is_space(line_[i])) {
            ++i;
        }
        if (i >= line_.size() || line_[i] != '#') {
            return;
        }
        const std::size_t hash = i++;
        while (i < line_.size() && is_space(line_[i])) {
            ++i;
        }
        const std::size_t word = i;
        while (i < line_.size() && is_word(line_[i])) {
            ++i;
        }
        emit(hash, i, Style::function);
        const std::string_view name = line_.substr(word, i - word);
        pos_ = i;
        if (name == "include" || name == "include_next" || name == "import" || name == "embed") {
            while (pos_ < line_.size() && is_space(line_[pos_])) {
                ++pos_;
            }
            if (pos_ < line_.size() && line_[pos_] == '<') {
                const std::size_t close = line_.find('>', pos_);
                const std::size_t end = close == std::string_view::npos ? line_.size() : close + 1;
                emit(pos_, end, Style::string);
                pos_ = end;
            }
        }
    }

    void step() {
        const char c = line_[pos_];
        if (!tables_.opens.at(static_cast<unsigned char>(c))) {
            ++pos_;  // a space, an operator, punctuation
            return;
        }
        if ((syntax_.keys == Keys::json || syntax_.keys == Keys::toml) && at_key_start() && key()) {
            return;
        }
        // Words first, the most common: no comment marker starts with a letter, and prefixed
        // and raw strings start as words.
        if (is_word_start(c)) {
            word();
            return;
        }
        // [==[ ends at ]==]: as many "=" between the brackets.
        const std::string_view long_comment = syntax_.long_comment;
        if (at(long_comment)) {
            if (const std::size_t length = long_bracket(line_.substr(pos_ + long_comment.size()))) {
                open_comment(pos_, long_comment.size() + length,
                             "]" + std::string(length - 2, '=') + "]");
                return;
            }
        }
        if (syntax_.long_strings) {
            if (const std::size_t length = long_bracket(line_.substr(pos_))) {
                open_string(pos_, pos_, "]" + std::string(length - 2, '=') + "]", false, true);
                return;
            }
        }
        if (c == '$' && syntax_.variables != Variables::none && variable()) {
            return;
        }
        if (syntax_.heredocs && c == '<' && next() == '<' && next(2) != '<' && heredoc()) {
            return;
        }
        for (const std::string_view marker : syntax_.line_comments) {
            if (at(marker) && (!syntax_.comment_at_word_start || at_word_start())) {
                emit(pos_, line_.size(), Style::comment);
                pos_ = line_.size();
                return;
            }
        }
        if (at(syntax_.block_open)) {
            open_comment(pos_, syntax_.block_open.size(), std::string{syntax_.block_close});
            return;
        }
        if (syntax_.rust_attributes && c == '#' &&
            (next() == '[' || (next() == '!' && next(2) == '['))) {
            attribute();
            return;
        }
        if (c == syntax_.decorator && is_word_start(next())) {
            decorator();
            return;
        }
        if (syntax_.lifetimes && c == '\'' && lifetime()) {
            return;
        }
        if (tables_.quotes.at(static_cast<unsigned char>(c))) {
            quoted(pos_, pos_, false);
            return;
        }
        // .5 is a number; the 10 of 0..10 starts at its digit.
        if (is_digit(c) ||
            (c == '.' && is_digit(next()) && (pos_ == 0 || line_[pos_ - 1] != '.'))) {
            number();
            return;
        }
        ++pos_;
    }

    // Where a shell word may start: the line's start, after a space or ; | & ( ).
    [[nodiscard]] bool at_word_start() const {
        if (pos_ == 0) {
            return true;
        }
        const char before = line_[pos_ - 1];
        return is_space(before) || before == ';' || before == '|' || before == '&' ||
               before == '(' || before == ')';
    }

    // Where a shell command starts, so NAME=value sets a variable: the line's start, after
    // ; | & ( {, or after export, local, readonly, declare, typeset.
    [[nodiscard]] bool at_command() const {
        std::size_t i = pos_;
        while (i > 0 && is_space(line_[i - 1])) {
            --i;
        }
        if (i == 0 || std::string_view{";|&({"}.find(line_[i - 1]) != std::string_view::npos) {
            return true;
        }
        std::size_t word = i;
        while (word > 0 && is_word(line_[word - 1])) {
            --word;
        }
        const std::string_view before = line_.substr(word, i - word);
        return before == "export" || before == "local" || before == "readonly" ||
               before == "declare" || before == "typeset";
    }

    // Just past the closing bracket matching the opening one at from (nesting counted); the
    // line's end when none.
    [[nodiscard]] std::size_t matching(std::size_t from, char open, char close) const {
        int depth = 0;
        for (std::size_t i = from; i < line_.size(); ++i) {
            if (line_[i] == open) {
                ++depth;
            } else if (line_[i] == close && --depth == 0) {
                return i + 1;
            }
        }
        return line_.size();
    }

    // $name and its kin, at pos_; false when the $ is just a character.
    bool variable() {
        const std::size_t start = pos_;
        const char after = next();
        const auto take = [&](std::size_t end, Style style) {
            pos_ = end;
            emit(start, end, style);
            return true;
        };
        switch (syntax_.variables) {
            case Variables::shell:
                if (after == '{') {
                    return take(matching(pos_ + 1, '{', '}'), Style::type);
                }
                if (after == '(') {
                    return false;  // $(command): lexed as code
                }
                if (after != '\0' && (is_digit(after) || std::string_view{"@#?$!*-"}.find(after) !=
                                                             std::string_view::npos)) {
                    return take(pos_ + 2, Style::type);
                }
                break;
            case Variables::make:
                if (after == '(' || after == '{') {
                    const char close = after == '(' ? ')' : '}';
                    std::size_t i = pos_ + 2;
                    while (i < line_.size() && (is_word(line_[i]) || line_[i] == '-')) {
                        ++i;
                    }
                    if (i < line_.size() && line_[i] != close) {
                        return take(i, Style::function);  // $(shell ...): a function
                    }
                    return take(matching(pos_ + 1, after, close), Style::type);
                }
                if (after != '\0' &&
                    std::string_view{"@<^*?%+|$"}.find(after) != std::string_view::npos) {
                    return take(pos_ + 2, Style::type);
                }
                break;
            case Variables::cmake:
                if (after == '{') {
                    return take(matching(pos_ + 1, '{', '}'), Style::type);
                }
                if (after == '<') {
                    return take(matching(pos_ + 1, '<', '>'), Style::function);
                }
                if (line_.substr(pos_ + 1).starts_with("ENV{") ||
                    line_.substr(pos_ + 1).starts_with("CACHE{")) {
                    return take(matching(line_.find('{', pos_), '{', '}'), Style::type);
                }
                return false;
            case Variables::none:
                return false;
        }
        if (is_word_start(after)) {
            std::size_t i = pos_ + 1;
            while (i < line_.size() && is_word(line_[i])) {
                ++i;
            }
            return take(i, Style::type);
        }
        return false;
    }

    // <<EOF, <<-EOF, <<'EOF', << "EOF": the operator and its word as a string; the lines from
    // the next one to the word alone on its line are the heredoc's.
    bool heredoc() {
        const std::size_t start = pos_;
        std::size_t i = pos_ + 2;
        const bool tabs = i < line_.size() && line_[i] == '-';
        i += tabs ? 1 : 0;
        while (i < line_.size() && is_space(line_[i])) {
            ++i;
        }
        const char quote = i < line_.size() ? line_[i] : '\0';
        const bool quoted = quote == '\'' || quote == '"';
        const bool escaped = quote == '\\';
        i += (quoted || escaped) ? 1 : 0;
        const std::size_t word = i;
        while (i < line_.size() && (is_word(line_[i]) || line_[i] == '-' || line_[i] == '.')) {
            ++i;
        }
        // << alone, or << 2 unquoted: a shift (in $(( ))), not a heredoc.
        if (i == word || (!quoted && is_digit(line_[word]))) {
            return false;
        }
        state_.heredoc = std::string{line_.substr(word, i - word)};
        state_.heredoc_tabs = tabs;
        if (quoted && i < line_.size() && line_[i] == quote) {
            ++i;
        }
        pos_ = i;
        emit(start, i, Style::string);
        return true;
    }

    // A line of a heredoc: a string, the last one its word alone (after tabs with <<-).
    void heredoc_line() {
        std::string_view bare = line_;
        if (state_.tabs) {
            bare.remove_prefix(std::min(bare.find_first_not_of('\t'), bare.size()));
        }
        emit(0, line_.size(), Style::string);
        pos_ = line_.size();
        if (bare == state_.close) {
            state_ = {};
        }
    }

    // Where a key may start in JSON and TOML: the line's start, after {, a comma or a comment.
    [[nodiscard]] bool at_key_start() const {
        std::size_t i = pos_;
        while (i > 0 && is_space(line_[i - 1])) {
            --i;
        }
        const std::string_view close = syntax_.block_close;
        return i == 0 || line_[i - 1] == '{' || line_[i - 1] == ',' ||
               (!close.empty() && line_.substr(0, i).ends_with(close));
    }

    // Just past a quoted key starting at i (a quote there); npos when it does not close on the
    // line.
    [[nodiscard]] std::size_t quoted_key(std::size_t i) const {
        const char q = line_[i];
        const bool escapes = syntax_.literal_quotes.find(q) == std::string_view::npos;
        for (++i; i < line_.size(); ++i) {
            if (line_[i] == '\\' && escapes) {
                ++i;
            } else if (line_[i] == q) {
                return i + 1;
            }
        }
        return std::string_view::npos;
    }

    // A key at pos_: "name": (JSON; JSON5 allows a bare name or 'name'), or name = and
    // a."b.c".d = (TOML), coloured as a keyword. False when what is there is not one.
    bool key() {
        const char separator = syntax_.keys == Keys::json ? ':' : '=';
        std::size_t i = pos_;
        while (true) {
            if (i < line_.size() && tables_.quotes.at(static_cast<unsigned char>(line_[i]))) {
                i = quoted_key(i);
                if (i == std::string_view::npos) {
                    return false;
                }
            } else {
                const std::size_t name = i;
                while (i < line_.size() &&
                       (is_word(line_[i]) || line_[i] == '-' || line_[i] == '$')) {
                    ++i;
                }
                if (i == name) {
                    return false;
                }
            }
            const std::size_t end = i;
            while (i < line_.size() && is_space(line_[i])) {
                ++i;
            }
            if (syntax_.keys == Keys::toml && i < line_.size() && line_[i] == '.') {
                ++i;
                while (i < line_.size() && is_space(line_[i])) {
                    ++i;
                }
                continue;
            }
            if (i < line_.size() && line_[i] == separator &&
                (i + 1 >= line_.size() || line_[i + 1] != '=')) {
                emit(pos_, end, Style::keyword);
                pos_ = end;
                return true;
            }
            return false;
        }
    }

    // [table] and [[array.of.tables]] alone on a line (a comment may follow): a type. An
    // array of arrays in a value, [1, 2], is not one: a key holds no commas.
    void toml_header() {
        std::size_t i = 0;
        while (i < line_.size() && is_space(line_[i])) {
            ++i;
        }
        if (i >= line_.size() || line_[i] != '[') {
            return;
        }
        const bool array = i + 1 < line_.size() && line_[i + 1] == '[';
        const std::size_t open = i + (array ? 2 : 1);
        const std::size_t close = line_.find(array ? "]]" : "]", open);
        if (close == std::string_view::npos || close == open) {
            return;
        }
        for (std::size_t k = open; k < close; ++k) {
            const char c = line_[k];
            if (!is_word(c) && std::string_view{" \t.-\"'"}.find(c) == std::string_view::npos) {
                return;
            }
        }
        const std::size_t end = close + (array ? 2 : 1);
        std::size_t rest = end;
        while (rest < line_.size() && is_space(line_[rest])) {
            ++rest;
        }
        if (rest < line_.size() && line_[rest] != '#') {
            return;
        }
        emit(i, end, Style::type);
        pos_ = end;
    }

    // An INI line (.desktop, systemd units, .gitconfig, .editorconfig): ; and # comments,
    // [section] headers, key= and key: (Name[it]=, keys with spaces); the value lexed as code.
    void ini_line() {
        std::size_t i = 0;
        while (i < line_.size() && is_space(line_[i])) {
            ++i;
        }
        if (i >= line_.size()) {
            return;
        }
        if (line_[i] == ';' || line_[i] == '#') {
            emit(i, line_.size(), Style::comment);
            pos_ = line_.size();
            return;
        }
        if (line_[i] == '[') {
            const std::size_t close = line_.rfind(']');
            if (close != std::string_view::npos && close > i) {
                emit(i, close + 1, Style::type);
                pos_ = close + 1;
                return;
            }
        }
        const std::size_t separator = line_.find_first_of("=:", i);
        if (separator == std::string_view::npos || separator == i) {
            return;
        }
        std::size_t end = separator;
        while (end > i && is_space(line_[end - 1])) {
            --end;
        }
        emit(i, end, Style::keyword);
        pos_ = separator;
    }

    // A Makefile line: "target: prerequisites" names a target, "NAME = value" (or :=, ?=, +=,
    // !=, ::=) sets a variable. Recipes (a tab first) and directives are lexed as code.
    void make_line() {
        if (line_.empty() || is_space(line_.front())) {
            return;
        }
        std::size_t i = 0;
        while (i < line_.size() && (is_word(line_[i]) || std::string_view{".-/%$(){}"}.find(
                                                             line_[i]) != std::string_view::npos)) {
            ++i;
        }
        const std::size_t name = i;
        while (i < line_.size() && is_space(line_[i])) {
            ++i;
        }
        if (name == 0 || i >= line_.size()) {
            return;
        }
        const std::string_view rest = line_.substr(i);
        const bool assignment = rest.starts_with('=') || rest.starts_with(":=") ||
                                rest.starts_with("::=") || rest.starts_with("?=") ||
                                rest.starts_with("+=") || rest.starts_with("!=");
        if (assignment) {
            emit(0, name, Style::type);
            pos_ = name;
        } else if (rest.starts_with(':') && !tables_.words.contains(line_.substr(0, name))) {
            emit(0, name, Style::function);
            pos_ = name;
        }
    }

    // A string opened by the quote at quote, its prefix from start; raw: no escapes.
    void quoted(std::size_t start, std::size_t quote, bool raw) {
        const char q = line_[quote];
        raw = raw || syntax_.literal_quotes.find(q) != std::string_view::npos;
        if (syntax_.triple_quotes && line_.substr(quote).starts_with(std::string(3, q))) {
            open_string(start, quote, std::string(3, q), !raw, true);
            return;
        }
        const bool multiline = syntax_.multiline_quotes.find(q) != std::string_view::npos;
        open_string(start, quote, std::string(1, q), !raw, multiline);
    }

    // R"delimiter( ... )delimiter" (C++): the prefix starts at start, the quote is at quote.
    bool cpp_raw(std::size_t start, std::size_t quote) {
        const std::size_t open = line_.find('(', quote + 1);
        if (open == std::string_view::npos || open - quote - 1 > 16) {
            return false;
        }
        const std::string_view delimiter = line_.substr(quote + 1, open - quote - 1);
        if (std::ranges::any_of(delimiter, [](char d) {
                return is_space(d) || d == '\\' || d == ')' || d == '"';
            })) {
            return false;
        }
        pos_ = open + 1;
        state_ = inside(State::In::string, ")" + std::string{delimiter} + "\"", false, 0);
        go_on_string(start);
        return true;
    }

    // r"...", r#"..."# (Rust): the prefix starts at start, the hashes or the quote at pos_.
    bool rust_raw(std::size_t start) {
        std::size_t i = pos_;
        while (i < line_.size() && line_[i] == '#') {
            ++i;
        }
        if (i >= line_.size() || line_[i] != '"') {
            return false;
        }
        const std::size_t hashes = i - pos_;
        pos_ = i + 1;
        state_ = inside(State::In::string, "\"" + std::string(hashes, '#'), false, 0);
        go_on_string(start);
        return true;
    }

    // #[attribute] and #![attribute], brackets inside counted, on one line.
    void attribute() {
        const std::size_t start = pos_;
        int depth = 0;
        while (pos_ < line_.size()) {
            const char c = line_[pos_++];
            if (c == '[') {
                ++depth;
            } else if (c == ']' && --depth == 0) {
                break;
            }
        }
        emit(start, pos_, Style::function);
    }

    // @name, @module.name
    void decorator() {
        const std::size_t start = pos_++;
        while (pos_ < line_.size() && (is_word(line_[pos_]) || line_[pos_] == '.')) {
            ++pos_;
        }
        emit(start, pos_, Style::function);
    }

    // 'a' and '\n' are characters; 'a and 'static lifetimes (Rust). False: neither, the quote
    // is left for strings.
    bool lifetime() {
        if (next() == '\\') {
            return false;  // an escaped character: a string
        }
        // One character (of one to four bytes) and a closing quote: a character.
        std::size_t after = pos_ + 2;
        while (after < line_.size() &&
               (static_cast<unsigned char>(line_[after]) & 0xC0U) == 0x80U) {
            ++after;
        }
        if (after < line_.size() && line_[after] == '\'') {
            return false;
        }
        if (!is_word_start(next())) {
            return false;
        }
        const std::size_t start = pos_++;
        while (pos_ < line_.size() && is_word(line_[pos_])) {
            ++pos_;
        }
        emit(start, pos_, Style::type);
        return true;
    }

    void number() {
        const std::size_t start = pos_;
        const bool hex =
            line_.substr(start).starts_with("0x") || line_.substr(start).starts_with("0X");
        while (pos_ < line_.size()) {
            const char c = line_[pos_];
            const char before = pos_ > start ? line_[pos_ - 1] : '\0';
            const bool exponent_sign =
                (c == '+' || c == '-') &&
                (((before == 'e' || before == 'E') && !hex) || before == 'p' || before == 'P');
            const bool separator = c == '\'' && syntax_.digit_separators &&
                                   pos_ + 1 < line_.size() && is_word(line_[pos_ + 1]);
            // 1979-05-27, 07:32:00, -08:00: between digits.
            const bool date = syntax_.dates && (c == '-' || c == ':' || c == '+') &&
                              is_digit(before) && is_digit(next());
            // A range, 0..10 (Rust, Lua's concatenation): the number ends before it.
            const bool range = c == '.' && next() == '.';
            if (range || (!is_word(c) && c != '.' && !exponent_sign && !separator && !date)) {
                break;
            }
            ++pos_;
        }
        emit(start, pos_, Style::number);
    }

    // Whether name, glued to a quote, is one of the syntax's string prefixes; raw when it
    // holds the raw letter.
    [[nodiscard]] bool string_prefix(std::string_view name, bool& raw) const {
        std::string folded{name};
        if (syntax_.prefix_any_case) {
            std::ranges::transform(folded, folded.begin(), lower);
        }
        raw = syntax_.raw_letter != '\0' && folded.find(syntax_.raw_letter) != std::string::npos;
        return std::ranges::find(syntax_.string_prefixes, folded) != syntax_.string_prefixes.end();
    }

    void word() {
        const std::size_t start = pos_;
        const bool command_start = syntax_.variables == Variables::shell && at_command();
        while (pos_ < line_.size() && is_word(line_[pos_])) {
            ++pos_;
        }
        const std::string_view name = line_.substr(start, pos_ - start);
        const char after = pos_ < line_.size() ? line_[pos_] : '\0';
        if (syntax_.raw_strings == RawStrings::cpp && after == '"' &&
            (name == "R" || name == "u8R" || name == "uR" || name == "UR" || name == "LR") &&
            cpp_raw(start, pos_)) {
            return;
        }
        if (syntax_.raw_strings == RawStrings::rust && (after == '"' || after == '#') &&
            (name == "r" || name == "br" || name == "cr") && rust_raw(start)) {
            return;
        }
        bool raw = false;
        if (tables_.quotes.at(static_cast<unsigned char>(after)) && string_prefix(name, raw)) {
            quoted(start, pos_, raw);
            return;
        }
        // NAME=value where a command could start: a shell variable set.
        if (command_start && after == '=') {
            emit(start, pos_, Style::type);
            return;
        }
        if (syntax_.bang_macros && after == '!' && next() != '=') {
            ++pos_;
            emit(start, pos_, Style::function);
            return;
        }
        if (const std::optional<Style> style = classify(name)) {
            emit(start, pos_, *style);
        }
    }

    // What a word is; nullopt for a plain name.
    [[nodiscard]] std::optional<Style> classify(std::string_view name) const {
        if (const auto found = tables_.words.find(name); found != tables_.words.end()) {
            return found->second;
        }
        if (syntax_.suffix_t_types && name.size() > 2 && name.ends_with("_t")) {
            return Style::type;
        }
        if (syntax_.capitals && std::ranges::none_of(name, is_lower) &&
            std::ranges::count_if(name, is_upper) >= 2) {
            return syntax_.capitals;
        }
        // Called: the next thing after spaces is "(".
        std::size_t after = pos_;
        while (after < line_.size() && is_space(line_[after])) {
            ++after;
        }
        if (after < line_.size() && line_[after] == '(') {
            return Style::function;
        }
        if (syntax_.camel_case_types && is_upper(name.front()) &&
            std::ranges::any_of(name, is_lower)) {
            return Style::type;
        }
        return std::nullopt;
    }

    // ---------------------------------------------------------------------------------------
    // YAML: keys, quoted strings over lines, block scalars (| and >, their lines a string while
    // indented more than their key), anchors and aliases, tags, documents' --- and ...;
    // plain values coloured only when a number or a constant.

    void yaml() {
        if (state_.in == State::In::block && block_line()) {
            return;
        }
        if (state_.in == State::In::string) {
            go_on_string(0);
        }
        if (state_.in != State::In::code) {
            return;
        }
        if (pos_ == 0 && yaml_line_start()) {
            return;
        }
        std::size_t node = 0;  // the column of the line's last key or "- ": a block's parent
        while (node < line_.size() && line_[node] == ' ') {
            ++node;
        }
        int flow = 0;  // inside [ ] or { }: commas and brackets end plain values
        while (pos_ < line_.size() && state_.in == State::In::code) {
            yaml_step(flow, node);
        }
    }

    // A block scalar's line: a string while indented more than its key (blank lines too);
    // false when the block has ended and the line is lexed as YAML.
    bool block_line() {
        const std::size_t first = line_.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) {
            pos_ = line_.size();
            return true;
        }
        if (first > state_.indent) {
            emit(first, line_.size(), Style::string);
            pos_ = line_.size();
            return true;
        }
        state_ = {};
        return false;
    }

    // ---, ... and %directives at a line's start; true when nothing is left to lex.
    bool yaml_line_start() {
        if (line_.starts_with('%')) {
            emit(0, line_.size(), Style::function);
            pos_ = line_.size();
            return true;
        }
        if ((line_.starts_with("---") || line_.starts_with("...")) &&
            (line_.size() == 3 || is_space(line_[3]))) {
            emit(0, 3, Style::keyword);
            pos_ = 3;
        }
        return false;
    }

    [[nodiscard]] static bool flow_character(char c) {
        return c == ',' || c == '[' || c == ']' || c == '{' || c == '}';
    }

    // Just past an anchor's, an alias's or a tag's name starting at from.
    [[nodiscard]] std::size_t yaml_name_end(std::size_t from, int flow) const {
        while (from < line_.size() && !is_space(line_[from]) &&
               (flow == 0 || !flow_character(line_[from]))) {
            ++from;
        }
        return from;
    }

    void yaml_step(int& flow, std::size_t& node) {
        const char c = line_[pos_];
        const char after = next();
        const bool spaced = after == '\0' || is_space(after);
        if (is_space(c)) {
            ++pos_;
        } else if (c == '#' && (pos_ == 0 || is_space(line_[pos_ - 1]))) {
            emit(pos_, line_.size(), Style::comment);
            pos_ = line_.size();
        } else if ((c == '-' || c == '?' || c == ':') && spaced) {
            if (c == '-') {
                node = pos_;
            }
            ++pos_;
        } else if (flow_character(c)) {
            if (c == '[' || c == '{') {
                ++flow;
            } else if ((c == ']' || c == '}') && flow > 0) {
                --flow;
            }
            ++pos_;
        } else if (c == '&' || c == '*') {
            const std::size_t end = yaml_name_end(pos_ + 1, flow);
            emit(pos_, end, Style::function);
            pos_ = end;
        } else if (c == '!') {
            const std::size_t end = yaml_name_end(pos_ + 1, flow);
            emit(pos_, end, Style::type);
            pos_ = end;
        } else if ((c == '|' || c == '>') && block_scalar(node)) {
            return;
        } else if (c == '"' || c == '\'') {
            yaml_quoted(flow, node);
        } else {
            plain(flow, node);
        }
    }

    // | and > with their indicators (|-, >+, |2), then nothing but a comment: the lines after
    // are a string while indented more than node. False when the | starts a plain value.
    bool block_scalar(std::size_t node) {
        std::size_t i = pos_ + 1;
        while (i < line_.size() && (is_digit(line_[i]) || line_[i] == '-' || line_[i] == '+')) {
            ++i;
        }
        std::size_t rest = i;
        while (rest < line_.size() && is_space(line_[rest])) {
            ++rest;
        }
        if (rest < line_.size() && (line_[rest] != '#' || rest == i)) {
            return false;
        }
        emit(rest, line_.size(), Style::comment);
        pos_ = line_.size();
        state_ = {};
        state_.in = State::In::block;
        state_.indent = node;
        return true;
    }

    // "..." (escapes) and '...' ('' a quote), over lines when not closed; a key when a colon
    // follows.
    void yaml_quoted(int flow, std::size_t& node) {
        const std::size_t start = pos_;
        const char q = line_[pos_];
        std::size_t i = pos_ + 1;
        std::size_t end = std::string_view::npos;
        while (i < line_.size()) {
            // \" in double quotes, '' in single ones: the quote stays inside.
            const bool escaped =
                (q == '"' && line_[i] == '\\') ||
                (q == '\'' && line_[i] == '\'' && i + 1 < line_.size() && line_[i + 1] == '\'');
            if (escaped) {
                i += 2;
            } else if (line_[i] == q) {
                end = i + 1;
                break;
            } else {
                ++i;
            }
        }
        if (end == std::string_view::npos) {
            emit(start, line_.size(), Style::string);
            pos_ = line_.size();
            state_ = inside(State::In::string, std::string(1, q), q == '"', 0);
            return;
        }
        pos_ = end;
        while (i + 1 < line_.size() && is_space(line_[i + 1])) {
            ++i;
        }
        const bool is_key = i + 1 < line_.size() && line_[i + 1] == ':' &&
                            (i + 2 >= line_.size() || is_space(line_[i + 2]) || flow > 0);
        if (is_key) {
            node = start;
        }
        emit(start, end, is_key ? Style::keyword : Style::string);
    }

    // A plain value or key: to ": ", " #", the line's end (and in flows a comma or bracket).
    void plain(int flow, std::size_t& node) {
        const std::size_t start = pos_;
        std::size_t i = pos_;
        while (i < line_.size()) {
            const char c = line_[i];
            if (c == ':' && (i + 1 >= line_.size() || is_space(line_[i + 1]) ||
                             (flow > 0 && flow_character(line_[i + 1])))) {
                break;
            }
            if ((c == '#' && is_space(line_[i - 1])) || (flow > 0 && flow_character(c))) {
                break;
            }
            ++i;
        }
        std::size_t end = i;
        while (end > start && is_space(line_[end - 1])) {
            --end;
        }
        pos_ = std::max(i, start + 1);
        if (i < line_.size() && line_[i] == ':') {
            node = start;
            emit(start, end, Style::keyword);
        } else if (yaml_scalar(line_.substr(start, end - start))) {
            emit(start, end, Style::number);
        }
    }

    // A number (12, -1.5e3, 0x1F, 0o17, .inf, 1_000, 2026-11-09, 12:30:00) or a constant
    // (true, null, ~, yes, off: YAML 1.1's too, which many files still write).
    [[nodiscard]] static bool yaml_scalar(std::string_view text) {
        std::string folded{text};
        std::ranges::transform(folded, folded.begin(), lower);
        for (const std::string_view constant :
             {"true", "false", "null", "~", "yes", "no", "on", "off"}) {
            if (folded == constant) {
                return true;
            }
        }
        std::string_view number = folded;
        if (number.starts_with('+') || number.starts_with('-')) {
            number.remove_prefix(1);
        }
        if (number == ".inf" || number == ".nan") {
            return true;
        }
        if (number.size() > 2 && (number.starts_with("0x") || number.starts_with("0o"))) {
            const std::string_view digits =
                number[1] == 'x' ? "0123456789abcdef_" : std::string_view{"01234567_"};
            return number.substr(2).find_first_not_of(digits) == std::string_view::npos;
        }
        if (number.empty() ||
            !(is_digit(number.front()) ||
              (number.front() == '.' && number.size() > 1 && is_digit(number[1])))) {
            return false;
        }
        return number.find_first_not_of("0123456789._:-+etz") == std::string_view::npos;
    }

    // ---------------------------------------------------------------------------------------
    // Markdown, highlighted only: headings, emphasis, code spans and fences (in their own
    // language when the fence names one we know), links, lists, quotes, HTML comments,
    // rules; a front matter in YAML (---) or TOML (+++) on the first line.

    // NOLINTNEXTLINE(misc-no-recursion): one level: a fence never holds Markdown
    void markdown() {
        if (state_.in == State::In::embedded) {
            embedded_line();
            return;
        }
        if (state_.in == State::In::comment) {
            go_on_comment(0);
            if (state_.in == State::In::comment) {
                return;
            }
            markdown_inline();
            return;
        }
        if (first_ && front_matter()) {
            return;
        }
        const bool after_text = state_.after_text;
        state_.after_text = false;
        std::size_t i = 0;
        while (i < line_.size() && is_space(line_[i])) {
            ++i;
        }
        if (i >= line_.size()) {
            return;
        }
        pos_ = i;
        if (fence_open()) {
            return;
        }
        if (i <= 3 && markdown_block(after_text)) {
            return;
        }
        list_marker();
        // A paragraph's line: what follows may underline it.
        state_.after_text = pos_ == i;
        markdown_inline();
    }

    // ---, +++ alone on the first line: the front matter, in YAML or TOML, to the same line.
    bool front_matter() {
        const std::string_view line = trimmed_end(line_);
        if (line != "---" && line != "+++") {
            return false;
        }
        emit(0, line.size(), Style::keyword);
        pos_ = line_.size();
        state_ = {};
        state_.in = State::In::embedded;
        state_.close = std::string{line};
        state_.embedded = language_for_fence(line == "---" ? "yaml" : "toml");
        state_.inner = std::make_unique<State>();
        return true;
    }

    [[nodiscard]] static std::string_view trimmed_end(std::string_view text) {
        while (!text.empty() && is_space(text.back())) {
            text.remove_suffix(1);
        }
        return text;
    }

    // ``` or ~~~ (three or more) at pos_, then the language's name: the lines up to a fence
    // as long, of the same character, are code.
    bool fence_open() {
        const char c = line_[pos_];
        if (c != '`' && c != '~') {
            return false;
        }
        std::size_t end = pos_;
        while (end < line_.size() && line_[end] == c) {
            ++end;
        }
        const std::size_t length = end - pos_;
        const std::string_view info = line_.substr(end);
        if (length < 3 || (c == '`' && info.find('`') != std::string_view::npos)) {
            return false;
        }
        // ```rust, ``` rust title, ```{.python}: the first word, braces and dot dropped.
        std::size_t word = 0;
        while (word < info.size() &&
               (is_space(info[word]) || info[word] == '{' || info[word] == '.')) {
            ++word;
        }
        std::size_t word_end = word;
        while (word_end < info.size() && !is_space(info[word_end]) && info[word_end] != '}' &&
               info[word_end] != ',') {
            ++word_end;
        }
        emit(pos_, line_.size(), Style::string);
        const Language* language = language_for_fence(info.substr(word, word_end - word));
        pos_ = line_.size();
        state_ = {};
        state_.in = State::In::embedded;
        state_.fence = c;
        state_.fence_length = length;
        state_.embedded = language;
        state_.inner = std::make_unique<State>();
        return true;
    }

    // A line inside a fence or the front matter: the closing line, or code.
    // NOLINTNEXTLINE(misc-no-recursion): one level: a fence never holds Markdown
    void embedded_line() {
        pos_ = line_.size();
        std::string_view bare = trimmed_end(line_);
        if (state_.fence == '\0') {
            if (bare == state_.close || (state_.close == "---" && bare == "...")) {
                emit(0, bare.size(), Style::keyword);
                state_ = {};
                return;
            }
        } else {
            const std::size_t start = std::min(bare.find_first_not_of(" \t"), bare.size());
            const std::string_view fence = bare.substr(start);
            if (fence.size() >= state_.fence_length &&
                fence.find_first_not_of(state_.fence) == std::string_view::npos) {
                emit(start, bare.size(), Style::string);
                state_ = {};
                return;
            }
        }
        if (state_.embedded == nullptr) {
            emit(0, line_.size(), Style::string);
            return;
        }
        const Syntax& syntax = *state_.embedded->syntax;
        LineLexer{syntax, tables_of(syntax), line_, *state_.inner, out_}.run();
    }

    // Headings, rules and setext underlines, quotes, link definitions: true when the line is
    // done.
    bool markdown_block(bool after_text) {
        const std::size_t start = pos_;
        const std::string_view rest = trimmed_end(line_.substr(start));
        const char c = rest.front();
        if (c == '#') {
            std::size_t hashes = 0;
            while (hashes < rest.size() && rest[hashes] == '#') {
                ++hashes;
            }
            if (hashes <= 6 && (hashes == rest.size() || is_space(rest[hashes]))) {
                emit(start, start + rest.size(), Style::keyword);
                pos_ = line_.size();
                return true;
            }
        }
        if (c == '>') {
            emit(start, start + rest.size(), Style::comment);
            pos_ = line_.size();
            return true;
        }
        // === under a paragraph; --- under one too, else a rule like *** and ___.
        if (c == '=' && after_text && rest.find_first_not_of('=') == std::string_view::npos) {
            emit(start, start + rest.size(), Style::keyword);
            pos_ = line_.size();
            return true;
        }
        if (c == '-' || c == '*' || c == '_') {
            const bool only =
                rest.find_first_not_of(std::string{c, ' ', '\t'}) == std::string_view::npos;
            const auto count = static_cast<std::size_t>(std::ranges::count(rest, c));
            if (only && count >= 3) {
                const bool underline =
                    c == '-' && after_text && rest.find_first_not_of('-') == std::string_view::npos;
                emit(start, start + rest.size(), underline ? Style::keyword : Style::comment);
                pos_ = line_.size();
                return true;
            }
        }
        // [label]: url "title"
        if (c == '[') {
            const std::size_t close = line_.find("]:", start);
            if (close != std::string_view::npos && close > start + 1) {
                std::size_t url = close + 2;
                while (url < line_.size() && is_space(line_[url])) {
                    ++url;
                }
                std::size_t end = url;
                while (end < line_.size() && !is_space(line_[end])) {
                    ++end;
                }
                emit(url, end, Style::function);
                pos_ = end;
                markdown_inline();
                return true;
            }
        }
        return false;
    }

    // - item, * item, + item, 1. item, 2) item, then [ ] or [x]: the markers as numbers.
    void list_marker() {
        const std::size_t start = pos_;
        std::size_t i = pos_;
        if (line_[i] == '-' || line_[i] == '*' || line_[i] == '+') {
            ++i;
        } else {
            while (i < line_.size() && i - start < 9 && is_digit(line_[i])) {
                ++i;
            }
            if (i == start || i >= line_.size() || (line_[i] != '.' && line_[i] != ')')) {
                return;
            }
            ++i;
        }
        if (i < line_.size() && !is_space(line_[i])) {
            return;
        }
        emit(start, i, Style::number);
        pos_ = i;
        while (pos_ < line_.size() && is_space(line_[pos_])) {
            ++pos_;
        }
        const std::string_view task = line_.substr(pos_);
        if (task.size() >= 3 && task[0] == '[' && task[2] == ']' &&
            (task[1] == ' ' || task[1] == 'x' || task[1] == 'X') &&
            (task.size() == 3 || is_space(task[3]))) {
            emit(pos_, pos_ + 3, Style::number);
            pos_ += 3;
        }
    }

    // Emphasis, code spans, links' addresses, autolinks, bare URLs, HTML comments in the rest
    // of the line. Each kind of opener that found no closer is not looked for again past where
    // it failed: a line of a thousand * stays linear.
    void markdown_inline() {
        std::array<std::size_t, 9> no_emphasis{};  // *, _, ~ by run length 1-3: where none closes
        no_emphasis.fill(std::string_view::npos);
        std::array<std::size_t, 17> no_code{};  // `` by run length
        no_code.fill(std::string_view::npos);
        bool no_paren = false;
        while (pos_ < line_.size() && state_.in == State::In::code) {
            const char c = line_[pos_];
            if (c == '\\') {
                pos_ += 2;
            } else if (c == '`') {
                code_span(no_code);
            } else if (c == '*' || c == '_' || c == '~') {
                emphasis(no_emphasis);
            } else if (c == ']' && next() == '(' && !no_paren) {
                const std::size_t close = line_.find(')', pos_ + 2);
                if (close == std::string_view::npos) {
                    no_paren = true;
                    ++pos_;
                } else {
                    emit(pos_ + 2, close, Style::function);
                    pos_ = close + 1;
                }
            } else if (c == '<' && line_.substr(pos_).starts_with("<!--")) {
                open_comment(pos_, 4, "-->");
            } else if (c == '<') {
                autolink();
            } else if (c == 'h' && (pos_ == 0 || !is_word(line_[pos_ - 1])) &&
                       (line_.substr(pos_).starts_with("https://") ||
                        line_.substr(pos_).starts_with("http://"))) {
                // To a space, a bracket or a quote (an HTML attribute's); trailing punctuation
                // is the sentence's, as GitHub reads it.
                std::size_t end = pos_;
                while (end < line_.size() && !is_space(line_[end]) &&
                       std::string_view{"<>)]\"'"}.find(line_[end]) == std::string_view::npos) {
                    ++end;
                }
                while (std::string_view{".,:;!?*_~"}.find(line_[end - 1]) !=
                       std::string_view::npos) {
                    --end;
                }
                emit(pos_, end, Style::function);
                pos_ = end;
            } else {
                ++pos_;
            }
        }
        pos_ = std::min(pos_, line_.size());
    }

    // `code`, ``co`de``: to a run of as many backticks.
    void code_span(std::array<std::size_t, 17>& none) {
        const std::size_t start = pos_;
        std::size_t end = pos_;
        while (end < line_.size() && line_[end] == '`') {
            ++end;
        }
        const std::size_t length = end - start;
        pos_ = end;
        if (length >= none.size() || end >= none.at(length)) {
            return;
        }
        for (std::size_t i = end; i < line_.size();) {
            if (line_[i] != '`') {
                ++i;
                continue;
            }
            std::size_t run = i;
            while (run < line_.size() && line_[run] == '`') {
                ++run;
            }
            if (run - i == length) {
                emit(start, run, Style::string);
                pos_ = run;
                return;
            }
            i = run;
        }
        none.at(length) = end;
    }

    // *a*, **a**, ***a***, _a_ (not inside words: snake_case), ~~a~~: one to three of the
    // same character, not followed by a space, closed by as many not after a space.
    void emphasis(std::array<std::size_t, 9>& none) {
        const std::size_t start = pos_;
        const char c = line_[pos_];
        std::size_t end = pos_;
        while (end < line_.size() && line_[end] == c) {
            ++end;
        }
        const std::size_t length = end - start;
        pos_ = end;
        std::size_t kind = 2;  // ~
        if (c == '*') {
            kind = 0;
        } else if (c == '_') {
            kind = 1;
        }
        const bool opens = end < line_.size() && !is_space(line_[end]) &&
                           (c != '_' || start == 0 || !is_word(line_[start - 1]));
        if (length > 3 || (c == '~' && length != 2) || !opens) {
            return;
        }
        std::size_t& failed = none.at((kind * 3) + length - 1);
        if (end >= failed) {
            return;
        }
        for (std::size_t i = end + 1; i < line_.size();) {
            if (line_[i] != c) {
                ++i;
                continue;
            }
            std::size_t run = i;
            while (run < line_.size() && line_[run] == c) {
                ++run;
            }
            const bool closes = run - i == length && !is_space(line_[i - 1]) &&
                                (c != '_' || run >= line_.size() || !is_word(line_[run]));
            if (closes) {
                emit(start, run, Style::type);
                pos_ = run;
                return;
            }
            i = run;
        }
        failed = end;
    }

    // <https://example.org>, <name@example.org>: a link when nothing in it is a space.
    void autolink() {
        const std::size_t close = line_.find('>', pos_ + 1);
        if (close == std::string_view::npos) {
            pos_ = line_.size();
            return;
        }
        const std::string_view inside = line_.substr(pos_ + 1, close - pos_ - 1);
        const bool link = !inside.empty() && std::ranges::none_of(inside, is_space) &&
                          (inside.find("://") != std::string_view::npos ||
                           inside.find('@') != std::string_view::npos);
        if (link) {
            emit(pos_, close + 1, Style::function);
            pos_ = close + 1;
        } else {
            ++pos_;
        }
    }

    // ---------------------------------------------------------------------------------------
    // Diffs: + and - lines, @@ hunks (their lines counted, so a removed "-- x" inside one is
    // a removed line and "--- a/x" after it a header), headers, a patch's mail headers.

    void diff() {
        pos_ = line_.size();
        if (state_.old_lines > 0 || state_.new_lines > 0) {
            if (hunk_line()) {
                return;
            }
            state_.old_lines = 0;
            state_.new_lines = 0;
        }
        if (line_.starts_with("@@")) {
            hunk_header();
            return;
        }
        static constexpr std::array<std::string_view, 17> kHeaders{
            "diff ",
            "index ",
            "--- ",
            "+++ ",
            "new file mode",
            "deleted file mode",
            "old mode",
            "new mode",
            "similarity index",
            "dissimilarity index",
            "rename from",
            "rename to",
            "copy from",
            "copy to",
            "Binary files",
            "Only in ",
            "From ",
        };
        // A patch mail's "---" before the files' list.
        if (line_ == "---") {
            emit(0, line_.size(), Style::keyword);
            return;
        }
        for (const std::string_view header : kHeaders) {
            if (line_.starts_with(header)) {
                emit(0, line_.size(), Style::keyword);
                if (header == "From ") {
                    // A mail starts (the next one, in a series): its message comes.
                    state_.mail = true;
                    state_.diff_seen = false;
                } else {
                    state_.diff_seen = true;
                }
                return;
            }
        }
        // A patch mail: From:, Subject:, Date: before the message.
        if (state_.mail && !state_.diff_seen) {
            for (const std::string_view key : {"From:", "Subject:", "Date:", "To:", "Cc:"}) {
                if (line_.starts_with(key)) {
                    emit(0, key.size() - 1, Style::keyword);
                    return;
                }
            }
            return;  // a patch's message: - and + are a list's, not changes
        }
        if (line_.starts_with('\\')) {
            emit(0, line_.size(), Style::comment);
        } else if (line_ == "-- ") {
            return;  // a patch mail's signature: git's version follows
        } else if (line_.starts_with('+')) {
            emit(0, line_.size(), Style::added);
        } else if (line_.starts_with('-')) {
            emit(0, line_.size(), Style::removed);
        }
    }

    // A line inside a hunk; false when it cannot be one (the hunk was shorter than it said).
    bool hunk_line() {
        const char c = line_.empty() ? ' ' : line_.front();
        if (c == '\\') {
            emit(0, line_.size(), Style::comment);
            return true;
        }
        if (c == '-' && state_.old_lines > 0) {
            emit(0, line_.size(), Style::removed);
            --state_.old_lines;
            return true;
        }
        if (c == '+' && state_.new_lines > 0) {
            emit(0, line_.size(), Style::added);
            --state_.new_lines;
            return true;
        }
        if (c == ' ' && state_.old_lines > 0 && state_.new_lines > 0) {
            --state_.old_lines;
            --state_.new_lines;
            return true;
        }
        return false;
    }

    // @@ -12,4 +12,6 @@ context: the ranges as a function, their counts kept (1 when left out);
    // a combined diff's @@@ is coloured but not counted.
    void hunk_header() {
        state_.diff_seen = true;
        const std::size_t close = line_.find("@@", 2);
        const std::size_t end = close == std::string_view::npos ? line_.size() : close + 2;
        emit(0, end, Style::function);
        if (line_.starts_with("@@@")) {
            return;
        }
        const auto count = [&](char sign) -> std::size_t {
            const std::size_t at = line_.find(sign, 2);
            if (at == std::string_view::npos || at >= end) {
                return 0;
            }
            std::size_t i = at + 1;
            while (i < end && is_digit(line_[i])) {
                ++i;
            }
            if (i >= end || line_[i] != ',') {
                return 1;
            }
            std::size_t lines = 0;
            for (++i; i < end && is_digit(line_[i]) && lines < (std::size_t{1} << 40U); ++i) {
                lines = (lines * 10) + static_cast<std::size_t>(line_[i] - '0');
            }
            return lines;
        };
        state_.old_lines = count('-');
        state_.new_lines = count('+');
    }

    // ---------------------------------------------------------------------------------------
    // .gitignore and its kin, .gitattributes: one pattern a line (no state between lines).

    void ignore_line(bool attributes) {
        pos_ = line_.size();
        if (line_.starts_with('#')) {
            emit(0, line_.size(), Style::comment);
            return;
        }
        std::size_t start = 0;
        if (line_.starts_with('!')) {
            emit(0, 1, Style::keyword);
            start = 1;
        }
        std::size_t end = line_.size();
        if (attributes) {
            // The pattern ends at the first space, or after its closing quote: "a b" text.
            end = start;
            if (end < line_.size() && line_[end] == '"') {
                const std::size_t close = line_.find('"', end + 1);
                end = close == std::string_view::npos ? line_.size() : close + 1;
            }
            while (end < line_.size() && !is_space(line_[end])) {
                end += line_[end] == '\\' ? std::size_t{2} : std::size_t{1};
            }
            end = std::min(end, line_.size());
        }
        pattern(start, end);
        if (attributes) {
            attribute_list(end);
        }
    }

    // A pattern in [from, to): wildcards as functions, a leading / (from the root only) and a
    // trailing one (folders only) as types; \ escapes the next character; trailing spaces
    // do not count.
    void pattern(std::size_t from, std::size_t to) {
        std::size_t last = to;
        while (last > from && is_space(line_[last - 1]) &&
               (last - 1 == from || line_[last - 2] != '\\')) {
            --last;
        }
        for (std::size_t i = from; i < last;) {
            const char c = line_[i];
            if (c == '\\') {
                i += 2;
            } else if (c == '*' || c == '?') {
                const std::size_t start = i;
                while (i < last && (line_[i] == '*' || line_[i] == '?')) {
                    ++i;
                }
                emit(start, i, Style::function);
            } else if (c == '[') {
                // [abc], [!a-z], []x]: a ] right after the opening (or its !) is a member.
                std::size_t close = i + 1;
                if (close < last && (line_[close] == '!' || line_[close] == '^')) {
                    ++close;
                }
                if (close < last && line_[close] == ']') {
                    ++close;
                }
                while (close < last && line_[close] != ']') {
                    ++close;
                }
                if (close < last) {
                    emit(i, close + 1, Style::function);
                    i = close + 1;
                } else {
                    ++i;
                }
            } else if (c == '/' && (i == from || i + 1 == last)) {
                emit(i, i + 1, Style::type);
                ++i;
            } else {
                ++i;
            }
        }
    }

    // text -diff !eol eol=lf: each name (with its - or !) as a key, the value after = as a
    // string.
    void attribute_list(std::size_t from) {
        std::size_t i = from;
        while (i < line_.size()) {
            while (i < line_.size() && is_space(line_[i])) {
                ++i;
            }
            const std::size_t start = i;
            while (i < line_.size() && !is_space(line_[i]) && line_[i] != '=') {
                ++i;
            }
            emit(start, i, Style::keyword);
            if (i < line_.size() && line_[i] == '=') {
                const std::size_t value = i + 1;
                while (i < line_.size() && !is_space(line_[i])) {
                    ++i;
                }
                emit(value, i, Style::string);
            }
        }
    }

    const Syntax& syntax_;
    const Tables& tables_;
    std::string_view line_;
    State& state_;
    std::vector<Span>& out_;
    bool first_{false};  // the file's first line (Markdown's front matter)
    std::size_t pos_{0};
};

// The program a #! line runs: #!/usr/bin/env -S python3 -u gives "python3".
std::string_view interpreter_of(std::string_view first_line) {
    if (!first_line.starts_with("#!")) {
        return {};
    }
    std::string_view command = first_line.substr(2);
    const auto next_word = [&] {
        const std::size_t begin = command.find_first_not_of(" \t");
        if (begin == std::string_view::npos) {
            return std::string_view{};
        }
        const std::size_t end = command.find_first_of(" \t", begin);
        const std::string_view word = command.substr(begin, end - begin);
        command = end == std::string_view::npos ? std::string_view{} : command.substr(end);
        return word;
    };
    std::string_view program = next_word();
    program = program.substr(program.rfind('/') + 1);
    if (program == "env") {
        program = next_word();
        while (program.starts_with('-')) {
            program = next_word();
        }
    }
    return program;
}

}  // namespace

const Language* language_for(std::string_view name, std::string_view mime,
                             std::string_view first_line) {
    const std::span<const Language> languages = all_languages();
    for (const Language& language : languages) {
        if (std::ranges::find(language.mime, mime) != language.mime.end()) {
            return &language;
        }
    }
    for (const Language& language : languages) {
        if (std::ranges::find(language.file_names, name) != language.file_names.end()) {
            return &language;
        }
    }
    // By the extension, unless the file looks like markup: a Qt Linguist .ts is XML.
    const std::size_t dot = name.rfind('.');
    if (dot != std::string_view::npos && dot > 0 && !first_line.starts_with('<')) {
        const std::string_view extension = name.substr(dot);
        for (const Language& language : languages) {
            if (std::ranges::find(language.extensions, extension) != language.extensions.end()) {
                return &language;
            }
        }
    }
    // The program its first line runs, its version allowed after the name: python3.12.
    const std::string_view program = interpreter_of(first_line);
    if (!program.empty()) {
        for (const Language& language : languages) {
            for (const std::string_view interpreter : language.interpreters) {
                if (program.starts_with(interpreter) &&
                    program.substr(interpreter.size()).find_first_not_of("0123456789.") ==
                        std::string_view::npos) {
                    return &language;
                }
            }
        }
    }
    return nullptr;
}

const Language* language_for_fence(std::string_view name) {
    if (name.empty() || name.size() > 32) {
        return nullptr;
    }
    std::string folded{name};
    std::ranges::transform(folded, folded.begin(), lower);
    for (const Language& language : all_languages()) {
        if (std::ranges::find(language.fence_names, folded) != language.fence_names.end()) {
            return &language;
        }
    }
    return nullptr;
}

std::string_view language_name(const Language& language) { return language.name; }

void highlight(rummage::text::Text& text, const Language& language) {
    text.language = language.name;
    text.spans.assign(text.lines.size(), {});
    // A minified file: its language said, its lines left plain.
    if (rummage::text::looks_minified(text)) {
        return;
    }
    const Tables& tables = tables_of(*language.syntax);
    State state;
    for (std::size_t i = 0; i < text.lines.size(); ++i) {
        LineLexer{*language.syntax, tables, text.lines[i], state, text.spans[i], i == 0}.run();
    }
}

void colour(rummage::text::Text& text, std::string_view name, std::string_view mime) {
    const Language* language =
        language_for(name, mime, text.lines.empty() ? std::string_view{} : text.lines.front());
    if (language != nullptr) {
        highlight(text, *language);
    }
}

}  // namespace rummage::formats::text
