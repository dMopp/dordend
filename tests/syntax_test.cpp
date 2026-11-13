#include "rummage/formats/text/syntax.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/text/text.hpp"

namespace text = rummage::text;
namespace family = rummage::formats::text;
using Lines = std::vector<std::string>;

// Lines printed when a check fails, one per line.
template <>
struct doctest::StringMaker<Lines> {
    static doctest::String convert(const Lines& lines) {
        std::string out;
        for (const std::string& line : lines) {
            out += "\n    " + line;
        }
        return out.c_str();
    }
};

namespace {

// A line with its coloured pieces marked: {k:if} keyword, {t:} type, {s:} string, {n:} number or
// constant, {c:} comment, {f:} function or macro, {a:} added, {r:} removed.
std::string marked(const std::string& line, const std::vector<text::Span>& spans) {
    static constexpr std::string_view kLetters = "ktsncfar";
    std::string out;
    std::size_t at = 0;
    for (const text::Span& span : spans) {
        REQUIRE(span.start >= at);
        REQUIRE(span.start + span.length <= line.size());
        out += line.substr(at, span.start - at);
        out += '{';
        out += kLetters[static_cast<std::size_t>(span.style)];
        out += ':';
        out += line.substr(span.start, span.length);
        out += '}';
        at = span.start + span.length;
    }
    out += line.substr(at);
    return out;
}

// lines coloured as the language of name and mime; each line marked.
Lines colour(const Lines& lines, std::string_view mime, std::string_view name = "file") {
    text::Text text;
    text.lines = lines;
    family::colour(text, name, mime);
    Lines out;
    for (std::size_t i = 0; i < text.lines.size(); ++i) {
        out.push_back(
            marked(text.lines[i], text.spans.empty() ? std::vector<text::Span>{} : text.spans[i]));
    }
    return out;
}

Lines cpp(const Lines& lines) { return colour(lines, "text/x-c++src"); }

}  // namespace

TEST_CASE("syntax: the language from the type, the name or the first line") {
    const auto name_of = [](std::string_view name, std::string_view mime,
                            std::string_view first = {}) -> std::string {
        const family::Language* language = family::language_for(name, mime, first);
        return language == nullptr ? "" : std::string{family::language_name(*language)};
    };
    CHECK(name_of("a.c", "text/x-csrc") == "C");
    CHECK(name_of("a.h", "text/x-chdr") == "C");
    CHECK(name_of("a.cpp", "text/x-c++src") == "C++");
    CHECK(name_of("a.hpp", "text/x-c++hdr") == "C++");
    CHECK(name_of("notes.txt", "text/plain").empty());
    CHECK(name_of("notes.txt", "text/plain", "#!/bin/frobnicate").empty());

    text::Text plain;
    plain.lines = {"int x;"};
    family::colour(plain, "notes.txt", "text/plain");
    CHECK(plain.language.empty());
    CHECK(plain.spans.empty());
    text::Text code;
    code.lines = {"int x;", "", "y"};
    family::colour(code, "a.c", "text/x-csrc");
    CHECK(code.language == "C");
    CHECK(code.spans.size() == 3);  // one per line, empty ones too
}

TEST_CASE("syntax: C++ words") {
    CHECK(cpp({"int main(int argc, char** argv) {"}) ==
          Lines{"{t:int} {f:main}({t:int} argc, {t:char}** argv) {"});
    CHECK(cpp({"    return std::string_view{name}.size();"}) ==
          Lines{"    {k:return} std::{t:string_view}{name}.{f:size}();"});
    CHECK(cpp({"constexpr std::uint32_t kMagic = 0x4E50;"}) ==
          Lines{"{k:constexpr} std::{t:uint32_t} kMagic = {n:0x4E50};"});
    CHECK(cpp({"if (ptr == nullptr && !done) throw Error{};"}) ==
          Lines{"{k:if} (ptr == {n:nullptr} && !done) {k:throw} {t:Error}{};"});
    // Macros in capitals, CamelCase types, names called with spaces before the parenthesis.
    CHECK(cpp({"TEST_CASE (\"x\") { Registry r; EINTR; }"}) ==
          Lines{"{f:TEST_CASE} ({s:\"x\"}) { {t:Registry} r; {f:EINTR}; }"});
    // Words with digits, a name of one capital (no CamelCase), non-ASCII names.
    CHECK(cpp({"auto x2 = T{}; int città;"}) == Lines{"{t:auto} x2 = T{}; {t:int} città;"});
}

TEST_CASE("syntax: C++ numbers") {
    CHECK(cpp({"a = 42 + 3.14f + 1e-9 + 0x1Fu + 0b1010 + 1'000'000 + .5 + 0x1p-3;"}) ==
          Lines{"a = {n:42} + {n:3.14f} + {n:1e-9} + {n:0x1Fu} + {n:0b1010} + {n:1'000'000} + "
                "{n:.5} + {n:0x1p-3};"});
    // In hexadecimal, e is a digit: 0x1e-3 is 0x1e minus 3.
    CHECK(cpp({"0x1e-3"}) == Lines{"{n:0x1e}-{n:3}"});
}

TEST_CASE("syntax: C++ strings and characters") {
    CHECK(
        cpp({R"(auto s = "a \"quoted\" word"; char c = '\''; char d = 'x';)"}) ==
        Lines{
            R"({t:auto} s = {s:"a \"quoted\" word"}; {t:char} c = {s:'\''}; {t:char} d = {s:'x'};)"});
    CHECK(cpp({R"(f(u8"é", L"w", u'c', U"x");)"}) ==
          Lines{R"({f:f}({s:u8"é"}, {s:L"w"}, {s:u'c'}, {s:U"x"});)"});
    // Not closed: to the end of the line, and the next line is code again.
    CHECK(cpp({"\"open", "int x;"}) == Lines{"{s:\"open}", "{t:int} x;"});
    // An escaped backslash before the closing quote.
    CHECK(cpp({R"("a\\" + b)"}) == Lines{R"({s:"a\\"} + b)"});
}

TEST_CASE("syntax: C++ raw strings span lines") {
    CHECK(cpp({R"~(auto r = R"(no \escape)";)~"}) ==
          Lines{R"~({t:auto} r = {s:R"(no \escape)"};)~"});
    CHECK(
        cpp({R"~(auto r = R"sql(SELECT ")";)~", "FROM t;", R"~()sql" + more;)~"}) ==
        Lines{R"~({t:auto} r = {s:R"sql(SELECT ")";})~", "{s:FROM t;}", R"~({s:)sql"} + more;)~"});
    CHECK(cpp({R"~(x = LR"(w)" u8R"--(a)--";)~"}) ==
          Lines{R"~(x = {s:LR"(w)"} {s:u8R"--(a)--"};)~"});
    // Not a raw string: R alone.
    CHECK(cpp({"R + 1"}) == Lines{"R + {n:1}"});
}

TEST_CASE("syntax: C++ comments span lines") {
    CHECK(cpp({"int x;  // the count"}) == Lines{"{t:int} x;  {c:// the count}"});
    CHECK(cpp({"a /* one */ b"}) == Lines{"a {c:/* one */} b"});
    CHECK(cpp({"/* first", "   second", "   third */ int x;", "int y;"}) ==
          Lines{"{c:/* first}", "{c:   second}", "{c:   third */} {t:int} x;", "{t:int} y;"});
    // Inside a string, comment markers are text; inside a comment, quotes are.
    CHECK(cpp({R"(s = "// not a comment";)"}) == Lines{R"(s = {s:"// not a comment"};)"});
    CHECK(cpp({R"(// it's "fine")", "int x;"}) == Lines{R"({c:// it's "fine"})", "{t:int} x;"});
    CHECK(cpp({"/**/ x"}) == Lines{"{c:/**/} x"});
}

TEST_CASE("syntax: C preprocessor") {
    CHECK(cpp({"#include <vector>"}) == Lines{"{f:#include} {s:<vector>}"});
    CHECK(cpp({"#include \"rummage/text/text.hpp\""}) ==
          Lines{"{f:#include} {s:\"rummage/text/text.hpp\"}"});
    CHECK(cpp({"  #  define MAX(a, b) ((a) > (b) ? (a) : (b))"}) ==
          Lines{"  {f:#  define} {f:MAX}(a, b) ((a) > (b) ? (a) : (b))"});
    CHECK(cpp({"#pragma once"}) == Lines{"{f:#pragma} once"});
    CHECK(cpp({"#if defined(__linux__) // linux"}) ==
          Lines{"{f:#if} {f:defined}(__linux__) {c:// linux}"});
    // Not at the start of a line: just a character.
    CHECK(cpp({"a # b"}) == Lines{"a # b"});
}

TEST_CASE("syntax: lines that are not code still colour safely") {
    // Bytes that are no C++: nothing crashes, every piece is inside its line.
    const Lines odd{"",
                    "'",
                    "\"",
                    "R\"",
                    "R\"(",
                    "/*",
                    "*/",
                    "0x",
                    "1e+",
                    "#",
                    "#include <",
                    "u8",
                    "L'",
                    "\\",
                    "'''",
                    "1'",
                    "R\"abcdefghijklmnopqrstuvwxyz(x)\""};
    const Lines out = cpp(odd);
    CHECK(out.size() == odd.size());
}

TEST_CASE("syntax: the language by extension and interpreter") {
    const auto name_of = [](std::string_view name, std::string_view mime,
                            std::string_view first = {}) -> std::string {
        const family::Language* language = family::language_for(name, mime, first);
        return language == nullptr ? "" : std::string{family::language_name(*language)};
    };
    CHECK(name_of("main.rs", "text/rust") == "Rust");
    CHECK(name_of("a.py", "text/x-python") == "Python");
    CHECK(name_of("a.py", "text/x-python3") == "Python");
    CHECK(name_of("a.js", "text/javascript") == "JavaScript");
    CHECK(name_of("a.lua", "text/x-lua") == "Lua");
    // Types the MIME database gets wrong or does not know: by the extension.
    CHECK(name_of("app.ts", "text/vnd.trolltech.linguist", "import x from 'y';") == "TypeScript");
    CHECK(name_of("app.ts", "text/vnd.trolltech.linguist", "<?xml version=\"1.0\"?>").empty());
    CHECK(name_of("view.tsx", "application/x-tiled-tsx", "export {};") == "TypeScript");
    CHECK(name_of("view.jsx", "text/plain") == "JavaScript");
    CHECK(name_of("plugin.luau", "text/plain") == "Luau");
    CHECK(name_of(".rs", "text/plain").empty());  // a hidden file, not an extension
    // The program after #!.
    CHECK(name_of("tool", "text/plain", "#!/usr/bin/env python3") == "Python");
    CHECK(name_of("tool", "text/plain", "#!/usr/bin/env -S python3 -u") == "Python");
    CHECK(name_of("tool", "text/plain", "#!/usr/bin/python3.12") == "Python");
    CHECK(name_of("tool", "text/plain", "#! /usr/bin/env node") == "JavaScript");
    CHECK(name_of("tool", "text/plain", "#!/usr/bin/lua5.4") == "Lua");
    CHECK(name_of("tool", "text/plain", "#!/usr/bin/pythonista").empty());
}

namespace {

Lines rust(const Lines& lines) { return colour(lines, "text/rust"); }

Lines python(const Lines& lines) { return colour(lines, "text/x-python3"); }

Lines js(const Lines& lines) { return colour(lines, "text/javascript"); }

Lines ts(const Lines& lines) { return colour(lines, "text/plain", "a.ts"); }

Lines lua(const Lines& lines) { return colour(lines, "text/x-lua"); }

}  // namespace

TEST_CASE("syntax: Rust") {
    CHECK(rust({"fn main() {"}) == Lines{"{k:fn} {f:main}() {"});
    CHECK(rust({"let v: Vec<u32> = vec![1, 2];"}) ==
          Lines{"{k:let} v: {t:Vec}<{t:u32}> = {f:vec!}[{n:1}, {n:2}];"});
    CHECK(rust({"#[derive(Debug, Clone)]"}) == Lines{"{f:#[derive(Debug, Clone)]}"});
    CHECK(rust({"#![allow(dead_code)]"}) == Lines{"{f:#![allow(dead_code)]}"});
    // Lifetimes and characters.
    CHECK(
        rust({"fn f<'a>(s: &'a str) -> Option<char> { Some('x') }"}) ==
        Lines{
            "{k:fn} f<{t:'a}>(s: &{t:'a} {t:str}) -> {t:Option}<{t:char}> { {n:Some}({s:'x'}) }"});
    CHECK(rust({R"(let c = '\''; let d = 'é'; let s: &'static str;)"}) ==
          Lines{R"({k:let} c = {s:'\''}; {k:let} d = {s:'é'}; {k:let} s: &{t:'static} {t:str};)"});
    // Raw, byte and plain strings; plain ones go on over lines.
    CHECK(rust({R"~(let r = r#"a "quoted" b"#;)~"}) ==
          Lines{R"~({k:let} r = {s:r#"a "quoted" b"#};)~"});
    CHECK(rust({R"(let b = b"bytes"; let c = b'a'; let r = br"\d";)"}) ==
          Lines{R"({k:let} b = {s:b"bytes"}; {k:let} c = {s:b'a'}; {k:let} r = {s:br"\d"};)"});
    CHECK(rust({"let s = \"one", "two\";"}) == Lines{"{k:let} s = {s:\"one}", "{s:two\"};"});
    CHECK(rust({R"~(let r = r##"one)~", R"~(two"## + x;)~"}) ==
          Lines{R"~({k:let} r = {s:r##"one})~", R"~({s:two"##} + x;)~"});
    // Comments nest.
    CHECK(rust({"/* a /* b */ c */ x"}) == Lines{"{c:/* a /* b */ c */} x"});
    CHECK(rust({"/* a /* b */", "c */ x"}) == Lines{"{c:/* a /* b */}", "{c:c */} x"});
    // Numbers, ranges, constants in capitals; != is no macro.
    CHECK(rust({"for i in 0..10 { x != 1_000u64 + 2.5e-3; MAX_SIZE }"}) ==
          Lines{"{k:for} i {k:in} {n:0}..{n:10} { x != {n:1_000u64} + {n:2.5e-3}; {n:MAX_SIZE} }"});
}

TEST_CASE("syntax: Python") {
    CHECK(python({"def greet(name: str) -> None:"}) ==
          Lines{"{k:def} {f:greet}(name: {t:str}) -> {n:None}:"});
    CHECK(python({"@app.route(\"/\")"}) == Lines{"{f:@app.route}({s:\"/\"})"});
    CHECK(python({R"(x = f"hi {name}" + r'\d' + b"\x00" + Rb'\w' + u"é")"}) ==
          Lines{R"(x = {s:f"hi {name}"} + {s:r'\d'} + {s:b"\x00"} + {s:Rb'\w'} + {s:u"é"})"});
    // Triple quotes over lines; one quote ends with its line.
    CHECK(python({R"(""")", "Docs, with 'quotes'.", R"(""" + x)"}) ==
          Lines{R"({s:"""})", R"({s:Docs, with 'quotes'.})", R"({s:"""} + x)"});
    CHECK(python({"s = 'open", "x = 1"}) == Lines{"s = {s:'open}", "x = {n:1}"});
    CHECK(python({"class Point(Base):  # a comment"}) ==
          Lines{"{k:class} {f:Point}({t:Base}):  {c:# a comment}"});
    CHECK(python({"self.ready = True; MAX = 10"}) ==
          Lines{"{k:self}.ready = {n:True}; {n:MAX} = {n:10}"});
    // A string before a word glued to it: not a prefix.
    CHECK(python({"print(name\"x\")"}) == Lines{"{f:print}(name{s:\"x\"})"});
}

TEST_CASE("syntax: JavaScript and TypeScript") {
    CHECK(js({"const x = `a ${b}`; let y = null ?? undefined;"}) ==
          Lines{"{k:const} x = {s:`a ${b}`}; {k:let} y = {n:null} ?? {n:undefined};"});
    CHECK(js({"const t = `one", "two`;"}) == Lines{"{k:const} t = {s:`one}", "{s:two`};"});
    CHECK(js({"/** docs */ export default function f() {}"}) ==
          Lines{"{c:/** docs */} {k:export} {k:default} {k:function} {f:f}() {}"});
    CHECK(ts({"function f(a: number): string {"}) ==
          Lines{"{k:function} {f:f}(a: {t:number}): {t:string} {"});
    CHECK(ts({"@Component({}) class App implements OnInit {}"}) ==
          Lines{"{f:@Component}({}) {k:class} {t:App} {k:implements} {t:OnInit} {}"});
}

TEST_CASE("syntax: Lua and Luau") {
    CHECK(lua({"local function f(x) return x .. 'y' end"}) ==
          Lines{"{k:local} {k:function} {f:f}(x) {k:return} x .. {s:'y'} {k:end}"});
    CHECK(lua({"x = nil -- a comment"}) == Lines{"x = {n:nil} {c:-- a comment}"});
    CHECK(lua({"--[[ long", "comment ]] x = 1"}) ==
          Lines{"{c:--[[ long}", "{c:comment ]]} x = {n:1}"});
    CHECK(lua({"--[==[ a ]] b ]==] y"}) == Lines{"{c:--[==[ a ]] b ]==]} y"});
    CHECK(lua({"s = [[raw", "text]] .. t[1]"}) == Lines{"s = {s:[[raw}", "{s:text]]} .. t[{n:1}]"});
    CHECK(lua({"s = [=[a]]b]=]"}) == Lines{"s = {s:[=[a]]b]=]}"});
    CHECK(colour({"local n: number = 1"}, "text/plain", "x.luau") ==
          Lines{"{k:local} n: {t:number} = {n:1}"});
}

namespace {

Lines shell(const Lines& lines) { return colour(lines, "application/x-shellscript"); }

Lines fish(const Lines& lines) { return colour(lines, "application/x-fishscript"); }

Lines cmake(const Lines& lines) { return colour(lines, "text/x-cmake"); }

Lines make(const Lines& lines) { return colour(lines, "text/x-makefile"); }

}  // namespace

TEST_CASE("syntax: shell, fish, CMake and Makefiles found") {
    const auto name_of = [](std::string_view name, std::string_view mime,
                            std::string_view first = {}) -> std::string {
        const family::Language* language = family::language_for(name, mime, first);
        return language == nullptr ? "" : std::string{family::language_name(*language)};
    };
    CHECK(name_of("build.sh", "application/x-shellscript") == "Shell");
    CHECK(name_of("run", "application/x-shellscript", "#!/bin/sh") == "Shell");
    CHECK(name_of("a.bash", "text/plain") == "Shell");
    CHECK(name_of(".bashrc", "text/plain") == "Shell");
    CHECK(name_of("template", "text/plain") == "Shell");  // xbps-src
    CHECK(name_of("PKGBUILD", "text/plain") == "Shell");
    CHECK(name_of("tool", "text/plain", "#!/usr/bin/env bash") == "Shell");
    CHECK(name_of("config.fish", "application/x-fishscript") == "fish");
    CHECK(name_of("tool", "text/plain", "#!/usr/bin/env fish") == "fish");
    CHECK(name_of("CMakeLists.txt", "text/x-cmake") == "CMake");
    CHECK(name_of("CMakeLists.txt", "text/plain") == "CMake");
    CHECK(name_of("Makefile", "text/x-makefile") == "Makefile");
    CHECK(name_of("rules.mk", "text/plain") == "Makefile");
}

TEST_CASE("syntax: shell") {
    CHECK(
        shell({"if [ -f \"$file\" ]; then echo ${HOME}/x $1 $#; fi"}) ==
        Lines{
            "{k:if} [ -f {s:\"$file\"} ]; {k:then} {f:echo} {t:${HOME}}/x {t:$1} {t:$#}; {k:fi}"});
    CHECK(shell({"for f in *.txt; do printf '%s\\n' \"$f\"; done  # list"}) ==
          Lines{"{k:for} f {k:in} *.txt; {k:do} {f:printf} {s:'%s\\n'} {s:\"$f\"}; {k:done}  "
                "{c:# list}"});
    // # inside a word is no comment; $(command) is code.
    CHECK(shell({"url=a#b; n=$(wc -l < f)"}) == Lines{"{t:url}=a#b; {t:n}=$(wc -l < f)"});
    // A second assignment on the same command (x=1 y=2 cmd) is not told from an argument.
    CHECK(shell({"x=$name y=true"}) == Lines{"{t:x}={t:$name} y={n:true}"});
    CHECK(shell({"pkgname=rummage; echo a=b"}) == Lines{"{t:pkgname}=rummage; {f:echo} a=b"});
    CHECK(shell({"export PATH=$HOME/bin; local n=1"}) ==
          Lines{"{f:export} {t:PATH}={t:$HOME}/bin; {f:local} {t:n}={n:1}"});
    // Strings go on over lines.
    CHECK(shell({"msg='one", "two' ; ok"}) == Lines{"{t:msg}={s:'one}", "{s:two'} ; ok"});
}

TEST_CASE("syntax: heredocs") {
    CHECK(shell({"cat <<EOF > out", "Hello $USER", "  EOF", "EOF", "echo done"}) ==
          Lines{"cat {s:<<EOF} > out", "{s:Hello $USER}", "{s:  EOF}", "{s:EOF}",
                "{f:echo} {k:done}"});
    // <<- lets the last line start with tabs; quoted words.
    CHECK(shell({"cat <<-'END'", "\ttext", "\tEND", "x"}) ==
          Lines{"cat {s:<<-'END'}", "{s:\ttext}", "{s:\tEND}", "x"});
    CHECK(shell({"cat << \"STOP\"", "a", "STOP"}) ==
          Lines{"cat {s:<< \"STOP\"}", "{s:a}", "{s:STOP}"});
    // A here-string and a shift are not heredocs.
    CHECK(shell({"cat <<< \"$x\"", "echo"}) == Lines{"cat <<< {s:\"$x\"}", "{f:echo}"});
    CHECK(shell({"echo $((1 << 2))"}) == Lines{"{f:echo} $(({n:1} << {n:2}))"});
}

TEST_CASE("syntax: fish") {
    CHECK(fish({"function greet --argument name", "    set -l msg \"hi $name\"", "end"}) ==
          Lines{"{k:function} greet --argument name", "    {f:set} -l msg {s:\"hi $name\"}",
                "{k:end}"});
    CHECK(fish({"if test -n $argv[1]; and not status is-interactive  # x"}) ==
          Lines{"{k:if} {f:test} -n {t:$argv}[{n:1}]; {k:and} {k:not} {f:status} is-interactive  "
                "{c:# x}"});
}

TEST_CASE("syntax: CMake") {
    CHECK(cmake({"add_library(rummage_text STATIC src/text.cpp)  # the module"}) ==
          Lines{"{f:add_library}(rummage_text {n:STATIC} src/text.cpp)  {c:# the module}"});
    CHECK(cmake({"if(NOT ${NAME}_FOUND AND $ENV{HOME})"}) ==
          Lines{"{k:if}({n:NOT} {t:${NAME}}{n:_FOUND} {n:AND} {t:$ENV{HOME}})"});
    CHECK(cmake({"set(x \"a ${b}\" ON)"}) == Lines{"{f:set}(x {s:\"a ${b}\"} {n:ON})"});
    CHECK(cmake({"target_link_libraries(t $<$<CONFIG:Debug>:asan>)"}) ==
          Lines{"{f:target_link_libraries}(t {f:$<$<CONFIG:Debug>:asan>})"});
    CHECK(cmake({"#[[ a", "comment ]] x"}) == Lines{"{c:#[[ a}", "{c:comment ]]} x"});
    CHECK(cmake({"message([==[raw]==])"}) == Lines{"{f:message}({s:[==[raw]==]})"});
}

TEST_CASE("syntax: Makefiles") {
    CHECK(make({"CC := gcc", "all: main.o util.o", "\t$(CC) -o $@ $^ $(shell pkg-config x)"}) ==
          Lines{"{t:CC} := gcc", "{f:all}: main.o util.o",
                "\t{t:$(CC)} -o {t:$@} {t:$^} {f:$(shell} pkg-config x)"});
    CHECK(make({"ifeq ($(OS),Linux)  # linux", "include common.mk", "endif"}) ==
          Lines{"{k:ifeq} ({t:$(OS)},Linux)  {c:# linux}", "{k:include} common.mk", "{k:endif}"});
    CHECK(make({".PHONY: clean", "%.o: %.c", "CFLAGS += -O2"}) ==
          Lines{"{f:.PHONY}: clean", "{f:%.o}: %.c", "{t:CFLAGS} += -O2"});
}

TEST_CASE("syntax: JSON, TOML, YAML and INI found") {
    const auto name_of = [](std::string_view name, std::string_view mime) -> std::string {
        const family::Language* language = family::language_for(name, mime, {});
        return language == nullptr ? "" : std::string{family::language_name(*language)};
    };
    CHECK(name_of("a.json", "application/json") == "JSON");
    CHECK(name_of("a.geojson", "application/geo+json") == "JSON");
    CHECK(name_of("tsconfig.jsonc", "text/plain") == "JSON");
    CHECK(name_of("flake.lock", "text/plain") == "JSON");
    CHECK(name_of("Cargo.toml", "application/toml") == "TOML");
    CHECK(name_of("Cargo.lock", "text/plain") == "TOML");
    CHECK(name_of("a.yml", "application/yaml") == "YAML");
    CHECK(name_of(".clang-format", "text/plain") == "YAML");
    CHECK(name_of("rummage.desktop", "application/x-desktop") == "INI");
    CHECK(name_of("sshd.service", "text/x-dbus-service") == "INI");
    CHECK(name_of("a.timer", "text/plain") == "INI");
    CHECK(name_of(".editorconfig", "text/plain") == "INI");
    // .conf and .cfg hold too many different languages to guess.
    CHECK(name_of("a.conf", "text/plain").empty());
    CHECK(name_of("a.cfg", "text/plain").empty());
}

namespace {

Lines json(const Lines& lines) { return colour(lines, "application/json"); }

Lines toml(const Lines& lines) { return colour(lines, "application/toml"); }

Lines yaml(const Lines& lines) { return colour(lines, "application/yaml"); }

Lines ini(const Lines& lines) { return colour(lines, "application/x-desktop"); }

}  // namespace

TEST_CASE("syntax: JSON") {
    CHECK(json({"{\"name\": \"rummage\", \"n\": -1.5e3, \"ok\": true, \"x\": null,"}) ==
          Lines{"{{k:\"name\"}: {s:\"rummage\"}, {k:\"n\"}: -{n:1.5e3}, {k:\"ok\"}: {n:true}, "
                "{k:\"x\"}: {n:null},"});
    // A quote escaped inside a key; strings in arrays are values.
    CHECK(json({"  \"a\\\"b\" : [\"c\", \"d\"]"}) ==
          Lines{"  {k:\"a\\\"b\"} : [{s:\"c\"}, {s:\"d\"}]"});
    // JSONC and JSON5: comments, names and single quotes.
    CHECK(json({"// settings", "{ editor: 'x', /* off */ \"b\": 1 }"}) ==
          Lines{"{c:// settings}", "{ {k:editor}: {s:'x'}, {c:/* off */} {k:\"b\"}: {n:1} }"});
}

TEST_CASE("syntax: TOML") {
    CHECK(toml({"[package]  # the crate", "name = \"rummage\"", "version.workspace = true"}) ==
          Lines{"{t:[package]}  {c:# the crate}", "{k:name} = {s:\"rummage\"}",
                "{k:version.workspace} = {n:true}"});
    CHECK(toml({"[[bin]]", "a.\"b.c\".d = 1_000", "build-deps = { cc = \"1\", x = 0x1F }"}) ==
          Lines{"{t:[[bin]]}", "{k:a.\"b.c\".d} = {n:1_000}",
                "{k:build-deps} = { {k:cc} = {s:\"1\"}, {k:x} = {n:0x1F} }"});
    // Dates and times are numbers; an array of arrays is no header.
    CHECK(toml({"when = 1979-05-27T07:32:00-08:00", "t = 07:32:00", "[1, 2],"}) ==
          Lines{"{k:when} = {n:1979-05-27T07:32:00-08:00}", "{k:t} = {n:07:32:00}",
                "[{n:1}, {n:2}],"});
    // Literal strings have no escapes; triple quotes go on over lines.
    CHECK(toml({"path = 'C:\\dir\\'", "text = \"\"\"", "a = \"b\"", "\"\"\" # end"}) ==
          Lines{"{k:path} = {s:'C:\\dir\\'}", "{k:text} = {s:\"\"\"}", "{s:a = \"b\"}",
                "{s:\"\"\"} {c:# end}"});
}

TEST_CASE("syntax: YAML") {
    CHECK(yaml({"---", "name: rummage  # app", "version: 0.5", "tags: [a, 1, true]"}) ==
          Lines{"{k:---}", "{k:name}: rummage  {c:# app}", "{k:version}: {n:0.5}",
                "{k:tags}: [a, {n:1}, {n:true}]"});
    // A URL is no key nor comment; a quoted key; anchors, aliases and tags.
    CHECK(yaml({"url: http://x.org/#top", "\"a b\": 'it''s'", "base: &b !!map {x: ~}", "<<: *b"}) ==
          Lines{"{k:url}: http://x.org/#top", "{k:\"a b\"}: {s:'it''s'}",
                "{k:base}: {f:&b} {t:!!map} {{k:x}: {n:~}}", "{k:<<}: {f:*b}"});
    // A list of mappings.
    CHECK(yaml({"- name: build", "  run: make -j8", "- -1"}) ==
          Lines{"- {k:name}: build", "  {k:run}: make -j8", "- {n:-1}"});
}

TEST_CASE("syntax: YAML block scalars") {
    // Lines more indented than the key are the string, blank ones too; a # inside is text.
    CHECK(yaml({"script: |  # keep", "  echo a  # not a comment", "", "    b", "next: 1"}) ==
          Lines{"{k:script}: |  {c:# keep}", "  {s:echo a  # not a comment}", "", "    {s:b}",
                "{k:next}: {n:1}"});
    // Under a list item the key's column counts, not the dash's.
    CHECK(yaml({"- run: >-", "    folded", "  name: x"}) ==
          Lines{"- {k:run}: >-", "    {s:folded}", "  {k:name}: x"});
    // A | inside a value is no block.
    CHECK(yaml({"cmd: a | b", "x: 1"}) == Lines{"{k:cmd}: a | b", "{k:x}: {n:1}"});
    // Double-quoted strings go on over lines.
    CHECK(yaml({"msg: \"one", "two\": x", "y: 2"}) ==
          Lines{"{k:msg}: {s:\"one}", "{s:two\"}: x", "{k:y}: {n:2}"});
}

TEST_CASE("syntax: INI and desktop entries") {
    CHECK(ini({"[Desktop Entry]", "Name=rummage", "Name[it]=rummage", "Terminal=false", "# comment",
               "; another", "Exec=rummage %U # not a comment"}) ==
          Lines{"{t:[Desktop Entry]}", "{k:Name}=rummage", "{k:Name[it]}=rummage",
                "{k:Terminal}={n:false}", "{c:# comment}", "{c:; another}",
                "{k:Exec}=rummage %U # not a comment"});
    CHECK(colour({"[remote \"origin\"]", "\turl = git@github.com:a/b", "[Service]",
                  "ExecStart=/usr/bin/x --n=2"},
                 "text/plain",
                 ".gitconfig") == Lines{"{t:[remote \"origin\"]}", "\t{k:url} = git@github.com:a/b",
                                        "{t:[Service]}", "{k:ExecStart}=/usr/bin/x --n={n:2}"});
}

TEST_CASE("syntax: ignore files and .gitattributes") {
    const auto ignore = [](const Lines& lines) {
        return colour(lines, "text/plain", ".gitignore");
    };
    CHECK(ignore({"# build output", "/build/", "*.o", "!keep.o", "src/**/gen-?.[ch]",
                  "\\#not-a-comment", "a\\*b", "logs/  "}) ==
          Lines{"{c:# build output}", "{t:/}build{t:/}", "{f:*}.o", "{k:!}keep.o",
                "src/{f:**}/gen-{f:?}.{f:[ch]}", "\\#not-a-comment", "a\\*b", "logs{t:/}  "});
    // A [ never closed, [!a] and []x].
    CHECK(ignore({"a[b", "[!a]x", "[]x]"}) == Lines{"a[b", "{f:[!a]}x", "{f:[]x]}"});
    CHECK(colour({"# x"}, "text/plain", ".dockerignore") == Lines{"{c:# x}"});

    const auto attributes = [](const Lines& lines) {
        return colour(lines, "text/plain", ".gitattributes");
    };
    CHECK(attributes({"*.png binary", "*.sh  text eol=lf -diff !merge", "\"a b\" text", "# c"}) ==
          Lines{"{f:*}.png {k:binary}", "{f:*}.sh  {k:text} {k:eol}={s:lf} {k:-diff} {k:!merge}",
                "\"a b\" {k:text}", "{c:# c}"});
}

namespace {

Lines markdown(const Lines& lines) { return colour(lines, "text/markdown"); }

Lines diff(const Lines& lines) { return colour(lines, "text/x-patch"); }

}  // namespace

TEST_CASE("syntax: a minified file keeps its language, not its colours") {
    text::Text text;
    text.lines = {"var a=1;" + std::string(600, ' ') + "function f(){return 2}"};
    family::colour(text, "app.min.js", "text/javascript");
    CHECK(text.language == "JavaScript");
    REQUIRE(text.spans.size() == 1);
    CHECK(text.spans.front().empty());
}

TEST_CASE("syntax: Markdown and diffs found") {
    const auto name_of = [](std::string_view name, std::string_view mime) -> std::string {
        const family::Language* language = family::language_for(name, mime, {});
        return language == nullptr ? "" : std::string{family::language_name(*language)};
    };
    CHECK(name_of("README.md", "text/markdown") == "Markdown");
    CHECK(name_of("notes.mdown", "text/plain") == "Markdown");
    CHECK(name_of("a.patch", "text/x-patch") == "Diff");
    CHECK(name_of("a.rej", "text/x-reject") == "Diff");
}

TEST_CASE("syntax: Markdown blocks") {
    CHECK(markdown({"# rummage", "#hashtag", "> a quote", "", "---", "***"}) ==
          Lines{"{k:# rummage}", "#hashtag", "{c:> a quote}", "", "{c:---}", "{c:***}"});
    // Underlines: under a paragraph's line only.
    CHECK(markdown({"Title", "=====", "Other", "---", "", "==="}) ==
          Lines{"Title", "{k:=====}", "Other", "{k:---}", "", "==="});
    // Lists, tasks, a rule made of stars is no list.
    CHECK(markdown({"- one", "  * two", "1. three", "- [x] done", "- [ ] todo", "* * *",
                    "-not a list"}) == Lines{"{n:-} one", "  {n:*} two", "{n:1.} three",
                                             "{n:-} {n:[x]} done", "{n:-} {n:[ ]} todo",
                                             "{c:* * *}", "-not a list"});
    CHECK(markdown({"[home]: https://rummage.org \"Home\""}) ==
          Lines{"[home]: {f:https://rummage.org} \"Home\""});
}

TEST_CASE("syntax: Markdown inline") {
    CHECK(markdown({"Some *it*, **bold**, ~~gone~~ and `code` here."}) ==
          Lines{"Some {t:*it*}, {t:**bold**}, {t:~~gone~~} and {s:`code`} here."});
    // snake_case is no emphasis, 2 * 3 * 4 neither, nor an emphasis never closed.
    CHECK(markdown({"a snake_case_name, 2 * 3 * 4, **open"}) ==
          Lines{"a snake_case_name, 2 * 3 * 4, **open"});
    CHECK(markdown({"``a ` b`` and \\*not\\*"}) == Lines{"{s:``a ` b``} and \\*not\\*"});
    CHECK(markdown({"See [the docs](docs/features.md), <https://x.org> or https://y.org."}) ==
          Lines{"See [the docs]({f:docs/features.md}), {f:<https://x.org>} or "
                "{f:https://y.org}."});
    CHECK(markdown({"<a href=\"https://x.org\">X</a>"}) ==
          Lines{"<a href=\"{f:https://x.org}\">X</a>"});
    CHECK(markdown({"a <!-- hidden", "still --> b *c*"}) ==
          Lines{"a {c:<!-- hidden}", "{c:still -->} b {t:*c*}"});
}

TEST_CASE("syntax: Markdown code fences in their language") {
    CHECK(markdown({"```cpp", "int x = 1; // one", "```", "*after*"}) ==
          Lines{"{s:```cpp}", "{t:int} x = {n:1}; {c:// one}", "{s:```}", "{t:*after*}"});
    // A fence the language is not known for, a longer closing one, ~~~, a fence in a list.
    CHECK(markdown({"```brainfuck", "+[>]", "`````", "~~~ {.python}", "def f(): pass", "~~~"}) ==
          Lines{"{s:```brainfuck}", "{s:+[>]}", "{s:`````}", "{s:~~~ {.python}}",
                "{k:def} {f:f}(): {k:pass}", "{s:~~~}"});
    CHECK(markdown({"- step:", "  ```sh", "  echo hi", "  ```"}) ==
          Lines{"{n:-} step:", "  {s:```sh}", "  {f:echo} hi", "  {s:```}"});
    // The language's state goes on across the block's lines; a fence never closed holds the
    // rest of the file.
    CHECK(
        markdown({"```Rust", "/* a", "b */", "```", "```", "# not a heading"}) ==
        Lines{"{s:```Rust}", "{c:/* a}", "{c:b */}", "{s:```}", "{s:```}", "{s:# not a heading}"});
}

TEST_CASE("syntax: Markdown front matter") {
    CHECK(markdown({"---", "title: rummage", "draft: false", "---", "# Hi"}) ==
          Lines{"{k:---}", "{k:title}: rummage", "{k:draft}: {n:false}", "{k:---}", "{k:# Hi}"});
    CHECK(markdown({"+++", "title = \"x\"", "+++"}) ==
          Lines{"{k:+++}", "{k:title} = {s:\"x\"}", "{k:+++}"});
    // Only on the first line.
    CHECK(markdown({"text", "---", "a: b"}) == Lines{"text", "{k:---}", "a: b"});
}

TEST_CASE("syntax: diffs") {
    CHECK(diff({"diff --git a/x b/x", "index 1..2 100644", "--- a/x", "+++ b/x",
                "@@ -1,3 +1,3 @@ int main()", " same", "-old", "+new", " same"}) ==
          Lines{"{k:diff --git a/x b/x}", "{k:index 1..2 100644}", "{k:--- a/x}", "{k:+++ b/x}",
                "{f:@@ -1,3 +1,3 @@} int main()", " same", "{r:-old}", "{a:+new}", " same"});
    // Inside a hunk "--- x" is a removed line; after it, a header again.
    // Without headers (a fragment, a Markdown fence's), + and - lines are still changes.
    CHECK(diff({"-old", "+new", " same"}) == Lines{"{r:-old}", "{a:+new}", " same"});
    CHECK(markdown({"```diff", "-old", "+new", "```"}) ==
          Lines{"{s:```diff}", "{r:-old}", "{a:+new}", "{s:```}"});
    CHECK(diff({"@@ -1 +1 @@", "--- removed", "+++ added", "--- a/y", "+++ b/y",
                "\\ No newline at end of file"}) ==
          Lines{"{f:@@ -1 +1 @@}", "{r:--- removed}", "{a:+++ added}", "{k:--- a/y}", "{k:+++ b/y}",
                "{c:\\ No newline at end of file}"});
}

TEST_CASE("syntax: git's patch mails") {
    CHECK(diff({"From 9c235a7 Mon Sep 17 00:00:00 2001", "From: A <a@b>", "Subject: [PATCH] Fix",
                "", "- a list in the message", "---", " src/a.cpp | 2 +-", "diff --git a/a b/a",
                "@@ -1,2 +1,2 @@", "-a", "+b", " c", "-- ", "2.47.0"}) ==
          Lines{"{k:From 9c235a7 Mon Sep 17 00:00:00 2001}", "{k:From}: A <a@b>",
                "{k:Subject}: [PATCH] Fix", "", "- a list in the message", "{k:---}",
                " src/a.cpp | 2 +-", "{k:diff --git a/a b/a}", "{f:@@ -1,2 +1,2 @@}", "{r:-a}",
                "{a:+b}", " c", "-- ", "2.47.0"});
    // The next mail of a series: its message again.
    CHECK(diff({"@@ -1 +1 @@", "-a", "+b", "From 1 Mon Sep 17 00:00:00 2001", "- item"}) ==
          Lines{"{f:@@ -1 +1 @@}", "{r:-a}", "{a:+b}", "{k:From 1 Mon Sep 17 00:00:00 2001}",
                "- item"});
}
