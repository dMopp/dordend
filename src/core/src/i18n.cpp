#include "nordend/core/i18n.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <string_view>
#include <unordered_map>

namespace nordend::i18n {

std::span<const Entry> german_table();  // i18n_de.cpp

namespace {

Language from_environment() {
    // glibc's order for message catalogs: LANGUAGE (a list) unless the locale is C, then LC_ALL,
    // LC_MESSAGES, LANG.
    std::string_view locale;
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(name);  // NOLINT(concurrency-mt-unsafe): read at start
        if (value != nullptr && *value != '\0') {
            locale = value;
            break;
        }
    }
    if (locale == "C" || locale == "POSIX" || locale.starts_with("C.")) {
        return Language::english;
    }
    const char* list = std::getenv("LANGUAGE");  // NOLINT(concurrency-mt-unsafe)
    if (list != nullptr && *list != '\0' && !locale.empty()) {
        return language_of(list);
    }
    return language_of(locale);
}

std::atomic<Language>& current() {
    static std::atomic<Language> language{from_environment()};
    return language;
}

const std::unordered_map<std::string_view, std::string_view>& lookup(Language language) {
    static const auto german = [] {
        std::unordered_map<std::string_view, std::string_view> map;
        for (const Entry& entry : german_table()) {
            map.emplace(entry.english, entry.translated);
        }
        return map;
    }();
    static const std::unordered_map<std::string_view, std::string_view> none;
    return language == Language::german ? german : none;
}

}  // namespace

Language language() { return current().load(std::memory_order_relaxed); }

void set_language(Language language) { current().store(language, std::memory_order_relaxed); }

Language language_of(std::string_view locales) {
    while (!locales.empty()) {
        const auto colon = locales.find(':');
        const std::string_view locale = locales.substr(0, colon);
        if (locale == "de" || locale.starts_with("de_") || locale.starts_with("de.") ||
            locale.starts_with("de@")) {
            return Language::german;
        }
        if (locale == "en" || locale.starts_with("en_") || locale.starts_with("en.")) {
            return Language::english;
        }
        if (colon == std::string_view::npos) {
            break;
        }
        locales.remove_prefix(colon + 1);
    }
    return Language::english;
}

std::span<const Entry> table(Language language) {
    return language == Language::german ? german_table() : std::span<const Entry>{};
}

std::string_view tr(std::string_view english) {
    const auto& map = lookup(language());
    if (const auto it = map.find(english); it != map.end()) {
        return it->second;
    }
    return english;
}

std::string keys(std::string_view text) {
    if (language() != Language::german) {
        return std::string{text};
    }
    static constexpr std::array<Entry, 16> kGermanKeys{{
        {"Ctrl", "Strg"},
        {"Shift", "Umschalt"},
        {"Space", "Leertaste"},
        {"Delete", "Entf"},
        {"Backspace", "Rücktaste"},
        {"Enter", "Eingabe"},
        {"Escape", "Esc"},
        {"Home", "Pos1"},
        {"End", "Ende"},
        {"PgUp", "Bild↑"},
        {"PgDn", "Bild↓"},
        {"Left", "←"},
        {"Right", "→"},
        {"Arrows", "Pfeiltasten"},
        {"Menu", "Menü"},
        {"moves", "Bewegung"},
    }};
    std::string out;
    out.reserve(text.size() + 8);
    std::size_t i = 0;
    while (i < text.size()) {
        const auto separator = text.find_first_of("+ ,", i);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        const std::string_view word = text.substr(i, end - i);
        const auto* found = std::ranges::find(kGermanKeys, word, &Entry::english);
        out += found != kGermanKeys.end() ? found->translated : word;
        if (end < text.size()) {
            out += text[end];
        }
        i = end + 1;
    }
    return out;
}

std::string decimal(std::string text) {
    if (language() == Language::german) {
        for (std::size_t i = 1; i + 1 < text.size(); ++i) {
            const auto digit = [](char c) { return c >= '0' && c <= '9'; };
            if (text[i] == '.' && digit(text[i - 1]) && digit(text[i + 1])) {
                text[i] = ',';
            }
        }
    }
    return text;
}

}  // namespace nordend::i18n
