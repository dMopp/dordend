#include "nordend/core/i18n.hpp"

#include <doctest/doctest.h>
#include <set>
#include <string>
#include <string_view>

#include "nordend/views/format.hpp"

namespace i18n = nordend::i18n;
using i18n::Language;

namespace {

// Restores English, as every other test expects, when a test that switched language ends.
struct Speaking {
    explicit Speaking(Language language) { i18n::set_language(language); }
    ~Speaking() { i18n::set_language(Language::english); }
    Speaking(const Speaking&) = delete;
    Speaking& operator=(const Speaking&) = delete;
    Speaking(Speaking&&) = delete;
    Speaking& operator=(Speaking&&) = delete;
};

// The fields of a format string, "{}" and "{0}" alike, in a set of their numbers; automatic
// fields are numbered in order.
std::multiset<int> fields(std::string_view text) {
    std::multiset<int> out;
    int next = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{' && i + 1 < text.size() && text[i + 1] == '{') {
            ++i;
            continue;
        }
        if (text[i] != '{') {
            continue;
        }
        const auto close = text.find('}', i);
        REQUIRE(close != std::string_view::npos);
        const std::string_view inside = text.substr(i + 1, close - i - 1);
        const auto colon = inside.find(':');
        const std::string_view index = inside.substr(0, colon);
        out.insert(index.empty() ? next++ : std::stoi(std::string{index}));
        i = close;
    }
    return out;
}

}  // namespace

TEST_CASE("i18n: the language of a locale") {
    CHECK(i18n::language_of("de_DE.UTF-8") == Language::german);
    CHECK(i18n::language_of("de_AT@euro") == Language::german);
    CHECK(i18n::language_of("de") == Language::german);
    CHECK(i18n::language_of("en_US.UTF-8") == Language::english);
    CHECK(i18n::language_of("C") == Language::english);
    CHECK(i18n::language_of("") == Language::english);
    CHECK(i18n::language_of("fr:de:en") == Language::german);  // the first with a table
    CHECK(i18n::language_of("en:de") == Language::english);
}

TEST_CASE("i18n: every German entry keeps the fields of its English text") {
    std::set<std::string_view> seen;
    for (const i18n::Entry& entry : i18n::table(Language::german)) {
        CAPTURE(entry.english);
        CHECK(seen.insert(entry.english).second);  // no key twice
        CHECK_FALSE(entry.translated.empty());
        CHECK(fields(entry.english) == fields(entry.translated));
    }
}

TEST_CASE("i18n: English stays English") {
    CHECK(i18n::tr("Empty folder") == "Empty folder");
    CHECK(i18n::trf("{} of {} selected", 2, 5) == "2 of 5 selected");
    CHECK(i18n::keys("Ctrl+Shift+Z") == "Ctrl+Shift+Z");
    CHECK(nordend::views::format_size(1234) == "1.2 kB");
}

TEST_CASE("i18n: German") {
    const Speaking german{Language::german};
    CHECK(i18n::tr("Empty folder") == "Leerer Ordner");
    CHECK(i18n::tr("not in any table") == "not in any table");
    CHECK(i18n::trf("{} of {} selected", 2, 5) == "2 von 5 ausgewählt");
    CHECK(i18n::trf("Delete “{}” permanently?", "a.txt") == "„a.txt“ endgültig löschen?");
    CHECK(i18n::keys("Ctrl+Shift+Z") == "Strg+Umschalt+Z");
    CHECK(i18n::keys("Ctrl++  Ctrl+−") == "Strg++  Strg+−");
    CHECK(i18n::keys("Alt+Left") == "Alt+←");
    CHECK(i18n::keys("Space  Escape") == "Leertaste  Esc");
    CHECK(i18n::decimal("1.2 kB · 3.4 MB") == "1,2 kB · 3,4 MB");
    CHECK(i18n::decimal("v1.") == "v1.");
    CHECK(nordend::views::format_size(1) == "1 Byte");
    CHECK(nordend::views::format_size(999) == "999 Bytes");
    CHECK(nordend::views::format_size(18'100'000) == "18,1 MB");
}
