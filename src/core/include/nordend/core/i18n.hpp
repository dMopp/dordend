#pragma once

#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Translations of the interface. The English text is the key: code keeps its English strings,
// wrapped in tr() or trf(), and a language's table maps them to its own words. No gettext and no
// files to install: the tables are compiled in. The language comes from the environment once
// (LANGUAGE, LC_ALL, LC_MESSAGES, LANG, the way glibc picks message catalogs); a text a table
// does not have stays English.
namespace nordend::i18n {

enum class Language { english, german };

struct Entry {
    std::string_view english;
    std::string_view translated;
};

// The language the interface speaks: the first language of the environment that has a table.
[[nodiscard]] Language language();

// Overrides the environment's choice (tests, a future setting).
void set_language(Language language);

// The language of a POSIX locale or LANGUAGE list ("de_DE.UTF-8", "de:en"); english when none of
// them has a table. "C" and "POSIX" are english.
[[nodiscard]] Language language_of(std::string_view locales);

// A language's table (empty for english).
[[nodiscard]] std::span<const Entry> table(Language language);

// english in the interface's language.
[[nodiscard]] std::string_view tr(std::string_view english);

// std::format with a translated format string. A translation may reorder its arguments with
// numbered fields ("{1} von {0}"); a translation whose fields do not fit falls back to English.
template <class... Args>
[[nodiscard]] std::string trf(std::string_view english, const Args&... args) {
    const std::string_view text = tr(english);
    if (text.data() != english.data()) {
        try {
            return std::vformat(text, std::make_format_args(args...));
        } catch (const std::format_error&) {
            // The table's fault: English is still right.
        }
    }
    return std::vformat(english, std::make_format_args(args...));
}

// Key names in a shortcut the way the language writes them on keyboards ("Ctrl+Shift+Z" ->
// "Strg+Umschalt+Z"); the key names are words between "+", spaces and commas.
[[nodiscard]] std::string keys(std::string_view text);

// A number written with a decimal point ("1.5 MB") the way the language writes it ("1,5 MB").
[[nodiscard]] std::string decimal(std::string text);

}  // namespace nordend::i18n
