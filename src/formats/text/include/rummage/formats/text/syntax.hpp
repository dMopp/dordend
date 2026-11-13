#pragma once

#include <string_view>

#include "rummage/text/text.hpp"

// Syntax colours for the text family (ROADMAP 3b-2): which language a file is in, and its lines'
// coloured pieces. Our own lexers, small: one driven by a table per language (keywords, comments,
// strings, numbers), with a state carried from line to line for comments and strings that span
// several. Linked into the base engine, never into rummage.
namespace rummage::formats::text {

struct Language;

// The language of a file: from its MIME type, else its name (Makefile, CMakeLists.txt), else the
// interpreter its first line names (#!/usr/bin/env python3); nullptr when none is known.
[[nodiscard]] const Language* language_for(std::string_view name, std::string_view mime,
                                           std::string_view first_line);

// Its name for people: "C", "C++".
[[nodiscard]] std::string_view language_name(const Language& language);

// Colours text's lines as language, from its first line (text.spans gets one entry per line;
// text.language its name). A minified file (rummage::text::looks_minified) gets the name and
// empty spans.
void highlight(rummage::text::Text& text, const Language& language);

// Both: the language from name, mime and the first line, then the colours; text untouched when
// no language is known.
void colour(rummage::text::Text& text, std::string_view name, std::string_view mime);

}  // namespace rummage::formats::text
