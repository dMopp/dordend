#include "languages.hpp"

#include <array>
#include <string_view>

namespace rummage::formats::text {
namespace {

using rummage::text::Style;

// The word lists, one language at a time. Kept out of clang-format: one word per line would hide
// them.
// clang-format off

// ---------------------------------------------------------------------------------------------
// C and C++
// ---------------------------------------------------------------------------------------------

constexpr std::array<std::string_view, 1> kSlashComments{"//"};

constexpr auto kCppKeywords = std::to_array<std::string_view>({
    "_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex", "_Generic", "_Imaginary",
    "_Noreturn", "_Pragma", "_Static_assert", "_Thread_local", "__asm__", "__attribute__",
    "__declspec", "__inline", "__restrict", "alignas", "alignof", "and", "and_eq", "asm",
    "break", "case", "catch", "class", "co_await", "co_return", "co_yield", "compl", "concept",
    "const", "const_cast", "consteval", "constexpr", "constinit", "continue", "decltype",
    "default", "delete", "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
    "final", "for", "friend", "goto", "if", "import", "inline", "module", "mutable", "namespace",
    "new", "noexcept", "not", "not_eq", "operator", "or", "or_eq", "override", "private",
    "protected", "public", "register", "reinterpret_cast", "requires", "restrict", "return",
    "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template", "this",
    "thread_local", "throw", "try", "typedef", "typeid", "typename", "union", "using",
    "virtual", "volatile", "while", "xor", "xor_eq",
});

constexpr auto kCppTypes = std::to_array<std::string_view>({
    "FILE", "array", "atomic", "auto", "bool", "byte", "char", "char16_t", "char32_t",
    "char8_t", "deque", "double", "float", "function", "int", "list", "long", "map",
    "optional", "pair", "set", "shared_ptr", "short", "signed", "span", "string",
    "string_view", "tuple", "unique_ptr", "unordered_map", "unordered_set", "unsigned",
    "variant", "vector", "void", "wchar_t", "weak_ptr",
});

constexpr auto kCppConstants = std::to_array<std::string_view>({
    "NULL", "false", "nullptr", "stderr", "stdout", "true",
});

constexpr auto kCppPrefixes = std::to_array<std::string_view>({"L", "U", "u", "u8"});

constexpr Syntax kCpp{
    .line_comments = kSlashComments,
    .block_open = "/*",
    .block_close = "*/",
    .quotes = "\"'",
    .string_prefixes = kCppPrefixes,
    .raw_strings = RawStrings::cpp,
    .preprocessor = true,
    .digit_separators = true,
    .keywords = kCppKeywords,
    .types = kCppTypes,
    .constants = kCppConstants,
    .camel_case_types = true,
    .suffix_t_types = true,
    .capitals = Style::function,
};

constexpr std::array<std::string_view, 2> kCMime{"text/x-csrc", "text/x-chdr"};
constexpr std::array<std::string_view, 2> kCppMime{"text/x-c++src", "text/x-c++hdr"};

// ---------------------------------------------------------------------------------------------
// Rust
// ---------------------------------------------------------------------------------------------

constexpr auto kRustKeywords = std::to_array<std::string_view>({
    "Self", "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum",
    "extern", "fn", "for", "if", "impl", "in", "let", "loop", "macro_rules", "match", "mod",
    "move", "mut", "pub", "ref", "return", "self", "static", "struct", "super", "trait", "type",
    "union", "unsafe", "use", "where", "while", "yield",
});

constexpr auto kRustTypes = std::to_array<std::string_view>({
    "bool", "char", "f32", "f64", "i128", "i16", "i32", "i64", "i8", "isize", "str", "u128",
    "u16", "u32", "u64", "u8", "usize",
});

constexpr auto kRustConstants = std::to_array<std::string_view>({
    "Err", "None", "Ok", "Some", "false", "true",
});

constexpr auto kRustPrefixes = std::to_array<std::string_view>({"b", "c"});

constexpr Syntax kRust{
    .line_comments = kSlashComments,
    .block_open = "/*",
    .block_close = "*/",
    .nested_comments = true,
    .quotes = "\"'",
    .multiline_quotes = "\"",
    .string_prefixes = kRustPrefixes,
    .raw_strings = RawStrings::rust,
    .rust_attributes = true,
    .bang_macros = true,
    .lifetimes = true,
    .keywords = kRustKeywords,
    .types = kRustTypes,
    .constants = kRustConstants,
    .camel_case_types = true,
    .capitals = Style::number,
};

constexpr std::array<std::string_view, 1> kRustMime{"text/rust"};
constexpr std::array<std::string_view, 1> kRustExtensions{".rs"};

// ---------------------------------------------------------------------------------------------
// Python
// ---------------------------------------------------------------------------------------------

constexpr std::array<std::string_view, 1> kHashComments{"#"};

constexpr auto kPythonKeywords = std::to_array<std::string_view>({
    "and", "as", "assert", "async", "await", "break", "class", "cls", "continue", "def", "del",
    "elif", "else", "except", "finally", "for", "from", "global", "if", "import", "in", "is",
    "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "self", "try", "while", "with",
    "yield",
});

constexpr auto kPythonTypes = std::to_array<std::string_view>({
    "bool", "bytearray", "bytes", "complex", "dict", "float", "frozenset", "int", "list",
    "object", "set", "str", "tuple", "type",
});

constexpr auto kPythonConstants = std::to_array<std::string_view>({
    "Ellipsis", "False", "None", "NotImplemented", "True", "__file__", "__name__",
});

constexpr auto kPythonPrefixes = std::to_array<std::string_view>({
    "b", "br", "f", "fr", "r", "rb", "rf", "t", "tr", "rt", "u",
});

constexpr Syntax kPython{
    .line_comments = kHashComments,
    .quotes = "\"'",
    .triple_quotes = true,
    .string_prefixes = kPythonPrefixes,
    .prefix_any_case = true,
    .raw_letter = 'r',
    .decorator = '@',
    .keywords = kPythonKeywords,
    .types = kPythonTypes,
    .constants = kPythonConstants,
    .camel_case_types = true,
    .capitals = Style::number,
};

constexpr std::array<std::string_view, 2> kPythonMime{"text/x-python3", "text/x-python"};
constexpr std::array<std::string_view, 2> kPythonExtensions{".py", ".pyi"};
constexpr std::array<std::string_view, 2> kPythonInterpreters{"python", "pypy"};

// ---------------------------------------------------------------------------------------------
// JavaScript and TypeScript
// ---------------------------------------------------------------------------------------------

constexpr auto kJsKeywords = std::to_array<std::string_view>({
    "abstract", "as", "async", "await", "break", "case", "catch", "class", "const", "continue",
    "debugger", "declare", "default", "delete", "do", "else", "enum", "export", "extends",
    "finally", "for", "from", "function", "get", "if", "implements", "import", "in", "infer",
    "instanceof", "interface", "is", "keyof", "let", "namespace", "new", "of", "override",
    "private", "protected", "public", "readonly", "return", "satisfies", "set", "static",
    "super", "switch", "this", "throw", "try", "type", "typeof", "var", "void", "while", "with",
    "yield",
});

constexpr auto kJsTypes = std::to_array<std::string_view>({
    "any", "bigint", "boolean", "never", "number", "object", "string", "symbol", "unknown",
});

constexpr auto kJsConstants = std::to_array<std::string_view>({
    "Infinity", "NaN", "false", "null", "true", "undefined",
});

constexpr Syntax kJavaScript{
    .line_comments = kSlashComments,
    .block_open = "/*",
    .block_close = "*/",
    .quotes = "\"'`",
    .multiline_quotes = "`",
    .decorator = '@',
    .keywords = kJsKeywords,
    .types = kJsTypes,
    .constants = kJsConstants,
    .camel_case_types = true,
    .capitals = Style::number,
};

constexpr std::array<std::string_view, 2> kJsMime{"text/javascript", "application/javascript"};
constexpr std::array<std::string_view, 4> kJsExtensions{".js", ".mjs", ".cjs", ".jsx"};
constexpr std::array<std::string_view, 3> kJsInterpreters{"node", "deno", "bun"};
constexpr std::array<std::string_view, 2> kTsMime{"text/x-typescript", "application/x-typescript"};
constexpr std::array<std::string_view, 4> kTsExtensions{".ts", ".mts", ".cts", ".tsx"};

// ---------------------------------------------------------------------------------------------
// Lua and Luau
// ---------------------------------------------------------------------------------------------

constexpr std::array<std::string_view, 1> kDashComments{"--"};

constexpr auto kLuaKeywords = std::to_array<std::string_view>({
    "and", "break", "continue", "do", "else", "elseif", "end", "export", "for", "function",
    "goto", "if", "in", "local", "not", "or", "repeat", "return", "then", "type", "until",
    "while",
});

constexpr auto kLuaTypes = std::to_array<std::string_view>({
    "any", "boolean", "never", "number", "string", "thread", "unknown", "userdata",
});

constexpr auto kLuaConstants = std::to_array<std::string_view>({"false", "nil", "true"});

constexpr Syntax kLua{
    .line_comments = kDashComments,
    .long_comment = "--",
    .long_strings = true,
    .quotes = "\"'`",
    .keywords = kLuaKeywords,
    .types = kLuaTypes,
    .constants = kLuaConstants,
    .camel_case_types = true,
    .capitals = Style::number,
};

constexpr std::array<std::string_view, 1> kLuaMime{"text/x-lua"};
constexpr std::array<std::string_view, 1> kLuaExtensions{".lua"};
constexpr std::array<std::string_view, 2> kLuaInterpreters{"lua", "luajit"};
constexpr std::array<std::string_view, 1> kLuauExtensions{".luau"};
constexpr std::array<std::string_view, 1> kLuauInterpreters{"luau"};

// ---------------------------------------------------------------------------------------------
// Shell (sh, bash, zsh) and fish
// ---------------------------------------------------------------------------------------------

constexpr auto kShellKeywords = std::to_array<std::string_view>({
    "case", "coproc", "do", "done", "elif", "else", "esac", "fi", "for", "function", "if", "in",
    "select", "then", "time", "until", "while",
});

constexpr auto kShellBuiltins = std::to_array<std::string_view>({
    "alias", "bg", "bind", "break", "builtin", "cd", "command", "continue", "declare", "echo",
    "eval", "exec", "exit", "export", "fg", "getopts", "hash", "jobs", "kill", "let", "local",
    "mapfile", "popd", "printf", "pushd", "pwd", "read", "readarray", "readonly", "return", "set",
    "shift", "shopt", "source", "test", "trap", "type", "typeset", "ulimit", "umask", "unalias",
    "unset", "wait",
});

constexpr auto kShellConstants = std::to_array<std::string_view>({"false", "true"});

constexpr Syntax kShell{
    .line_comments = kHashComments,
    .comment_at_word_start = true,
    .quotes = "\"'",
    .multiline_quotes = "\"'",
    .variables = Variables::shell,
    .heredocs = true,
    .keywords = kShellKeywords,
    .constants = kShellConstants,
    .functions = kShellBuiltins,
};

constexpr auto kFishKeywords = std::to_array<std::string_view>({
    "and", "begin", "break", "case", "continue", "else", "end", "for", "function", "if", "in",
    "not", "or", "return", "switch", "while",
});

constexpr auto kFishBuiltins = std::to_array<std::string_view>({
    "abbr", "alias", "argparse", "bind", "builtin", "cd", "command", "commandline", "complete",
    "contains", "count", "echo", "emit", "eval", "exec", "exit", "functions", "math", "printf",
    "read", "set", "set_color", "source", "status", "string", "test", "type",
});

constexpr Syntax kFish{
    .line_comments = kHashComments,
    .comment_at_word_start = true,
    .quotes = "\"'",
    .multiline_quotes = "\"'",
    .variables = Variables::shell,
    .keywords = kFishKeywords,
    .constants = kShellConstants,
    .functions = kFishBuiltins,
};

constexpr std::array<std::string_view, 1> kShellMime{"application/x-shellscript"};
constexpr std::array<std::string_view, 4> kShellExtensions{".sh", ".bash", ".zsh", ".ksh"};
constexpr std::array<std::string_view, 12> kShellNames{
    ".bashrc", ".bash_profile", ".bash_logout", ".bash_aliases", ".profile", ".zshrc",
    ".zprofile", ".zshenv", ".envrc", "PKGBUILD", "APKBUILD", "template",
};
constexpr std::array<std::string_view, 8> kShellInterpreters{
    "sh", "bash", "dash", "zsh", "ksh", "mksh", "ash", "busybox",
};
constexpr std::array<std::string_view, 1> kFishMime{"application/x-fishscript"};
constexpr std::array<std::string_view, 1> kFishExtensions{".fish"};
constexpr std::array<std::string_view, 1> kFishInterpreters{"fish"};

// ---------------------------------------------------------------------------------------------
// CMake and Makefiles
// ---------------------------------------------------------------------------------------------

constexpr auto kCMakeKeywords = std::to_array<std::string_view>({
    "block", "break", "continue", "else", "elseif", "endblock", "endforeach", "endfunction",
    "endif", "endmacro", "endwhile", "foreach", "function", "if", "macro", "return", "while",
});

constexpr auto kCMakeConstants = std::to_array<std::string_view>({
    "FALSE", "IGNORE", "N", "NO", "NOTFOUND", "OFF", "ON", "TRUE", "Y", "YES",
});

constexpr Syntax kCMake{
    .line_comments = kHashComments,
    .long_comment = "#",
    .long_strings = true,
    .quotes = "\"",
    .multiline_quotes = "\"",
    .variables = Variables::cmake,
    .keywords = kCMakeKeywords,
    .constants = kCMakeConstants,
    .capitals = Style::number,
};

constexpr auto kMakeKeywords = std::to_array<std::string_view>({
    "-include", "define", "else", "endef", "endif", "export", "ifdef", "ifeq", "ifndef", "ifneq",
    "include", "override", "private", "sinclude", "undefine", "unexport", "vpath",
});

constexpr Syntax kMake{
    .line_comments = kHashComments,
    .quotes = "\"'",
    .variables = Variables::make,
    .make_targets = true,
    .keywords = kMakeKeywords,
};

constexpr std::array<std::string_view, 1> kCMakeMime{"text/x-cmake"};
constexpr std::array<std::string_view, 1> kCMakeExtensions{".cmake"};
constexpr std::array<std::string_view, 1> kCMakeNames{"CMakeLists.txt"};
constexpr std::array<std::string_view, 1> kMakeMime{"text/x-makefile"};
constexpr std::array<std::string_view, 2> kMakeExtensions{".mk", ".mak"};
constexpr std::array<std::string_view, 3> kMakeNames{"Makefile", "makefile", "GNUmakefile"};
constexpr std::array<std::string_view, 1> kMakeInterpreters{"make"};

// ---------------------------------------------------------------------------------------------
// JSON, TOML, YAML and INI
// ---------------------------------------------------------------------------------------------

// JSON with JSONC's and JSON5's comments, single quotes and names: plain JSON has none, so the
// one syntax reads all three.
constexpr auto kJsonConstants = std::to_array<std::string_view>({
    "Infinity", "NaN", "false", "null", "true",
});

constexpr Syntax kJson{
    .line_comments = kSlashComments,
    .block_open = "/*",
    .block_close = "*/",
    .quotes = "\"'",
    .keys = Keys::json,
    .constants = kJsonConstants,
};

constexpr auto kTomlConstants = std::to_array<std::string_view>({"false", "inf", "nan", "true"});

constexpr Syntax kToml{
    .line_comments = kHashComments,
    .quotes = "\"'",
    .literal_quotes = "'",
    .triple_quotes = true,
    .dates = true,
    .keys = Keys::toml,
    .constants = kTomlConstants,
};

constexpr Syntax kYaml{
    .dedicated = Dedicated::yaml,
};

constexpr auto kIniConstants = std::to_array<std::string_view>({"false", "true"});

constexpr Syntax kIni{
    .keys = Keys::ini,
    .constants = kIniConstants,
};

constexpr std::array<std::string_view, 7> kJsonMime{
    "application/json", "application/json5", "application/geo+json", "application/ld+json",
    "application/schema+json", "application/x-ipynb+json", "application/manifest+json",
};
constexpr std::array<std::string_view, 7> kJsonExtensions{
    ".json", ".jsonc", ".json5", ".jsonl", ".geojson", ".webmanifest", ".ipynb",
};
constexpr std::array<std::string_view, 2> kJsonNames{"flake.lock", "deno.lock"};
constexpr std::array<std::string_view, 2> kTomlMime{"application/toml", "text/x-toml"};
constexpr std::array<std::string_view, 1> kTomlExtensions{".toml"};
constexpr std::array<std::string_view, 4> kTomlNames{
    "Cargo.lock", "Pipfile", "poetry.lock", "uv.lock",
};
constexpr std::array<std::string_view, 3> kYamlMime{
    "application/yaml", "application/x-yaml", "text/x-yaml",
};
constexpr std::array<std::string_view, 2> kYamlExtensions{".yaml", ".yml"};
constexpr std::array<std::string_view, 4> kYamlNames{
    ".clang-format", ".clang-tidy", ".clangd", ".yamllint",
};
constexpr std::array<std::string_view, 2> kIniMime{"application/x-desktop", "text/x-dbus-service"};
constexpr std::array<std::string_view, 15> kIniExtensions{
    ".ini", ".desktop", ".directory", ".flatpakref", ".flatpakrepo",
    // systemd units
    ".service", ".socket", ".timer", ".mount", ".automount", ".path", ".target", ".slice",
    ".network", ".netdev",
};
constexpr Syntax kIgnore{
    .dedicated = Dedicated::ignore,
};

constexpr Syntax kAttributes{
    .dedicated = Dedicated::attributes,
};

constexpr std::array<std::string_view, 9> kIgnoreNames{
    ".gitignore", ".dockerignore", ".ignore", ".rgignore", ".fdignore", ".npmignore",
    ".prettierignore", ".eslintignore", ".stignore",
};
constexpr std::array<std::string_view, 1> kAttributesNames{".gitattributes"};
constexpr std::array<std::string_view, 4> kIniNames{
    ".editorconfig", ".gitconfig", ".gitmodules", "mimeapps.list",
};

// ---------------------------------------------------------------------------------------------
// Markdown and diffs
// ---------------------------------------------------------------------------------------------

constexpr Syntax kMarkdown{
    .dedicated = Dedicated::markdown,
};

constexpr Syntax kDiff{
    .dedicated = Dedicated::diff,
};

constexpr std::array<std::string_view, 2> kMarkdownMime{"text/markdown", "text/x-markdown"};
constexpr std::array<std::string_view, 4> kMarkdownExtensions{".md", ".markdown", ".mkd", ".mdown"};
constexpr std::array<std::string_view, 3> kDiffMime{
    "text/x-patch", "text/x-diff", "text/x-reject",
};
constexpr std::array<std::string_view, 3> kDiffExtensions{".diff", ".patch", ".rej"};

// The names code fences give each language, in lower case (```C++ finds "c++"). Markdown has
// none: a fence never holds Markdown coloured as such, so fences never nest.
constexpr std::array<std::string_view, 2> kCFences{"c", "h"};
constexpr std::array<std::string_view, 6> kCppFences{"cpp", "c++", "cc", "cxx", "hpp", "cuda"};
constexpr std::array<std::string_view, 2> kRustFences{"rust", "rs"};
constexpr std::array<std::string_view, 5> kPythonFences{"python", "py", "python3", "py3", "pyi"};
constexpr std::array<std::string_view, 6> kJsFences{
    "javascript", "js", "jsx", "mjs", "cjs", "node",
};
constexpr std::array<std::string_view, 3> kTsFences{"typescript", "ts", "tsx"};
constexpr std::array<std::string_view, 1> kLuaFences{"lua"};
constexpr std::array<std::string_view, 1> kLuauFences{"luau"};
constexpr std::array<std::string_view, 7> kShellFences{
    "sh", "bash", "shell", "zsh", "ksh", "shellscript", "dash",
};
constexpr std::array<std::string_view, 1> kFishFences{"fish"};
constexpr std::array<std::string_view, 1> kCMakeFences{"cmake"};
constexpr std::array<std::string_view, 3> kMakeFences{"make", "makefile", "mk"};
constexpr std::array<std::string_view, 4> kJsonFences{"json", "jsonc", "json5", "jsonl"};
constexpr std::array<std::string_view, 1> kTomlFences{"toml"};
constexpr std::array<std::string_view, 2> kYamlFences{"yaml", "yml"};
constexpr std::array<std::string_view, 5> kIniFences{
    "ini", "desktop", "systemd", "dosini", "gitconfig",
};
constexpr std::array<std::string_view, 1> kIgnoreFences{"gitignore"};
constexpr std::array<std::string_view, 1> kAttributesFences{"gitattributes"};
constexpr std::array<std::string_view, 3> kDiffFences{"diff", "patch", "udiff"};

// clang-format on

constexpr std::array kLanguages{
    Language{.name = "C", .mime = kCMime, .fence_names = kCFences, .syntax = &kCpp},
    Language{.name = "C++", .mime = kCppMime, .fence_names = kCppFences, .syntax = &kCpp},
    Language{.name = "Rust",
             .mime = kRustMime,
             .extensions = kRustExtensions,
             .fence_names = kRustFences,
             .syntax = &kRust},
    Language{.name = "Python",
             .mime = kPythonMime,
             .extensions = kPythonExtensions,
             .interpreters = kPythonInterpreters,
             .fence_names = kPythonFences,
             .syntax = &kPython},
    Language{.name = "JavaScript",
             .mime = kJsMime,
             .extensions = kJsExtensions,
             .interpreters = kJsInterpreters,
             .fence_names = kJsFences,
             .syntax = &kJavaScript},
    Language{.name = "TypeScript",
             .mime = kTsMime,
             .extensions = kTsExtensions,
             .fence_names = kTsFences,
             .syntax = &kJavaScript},
    Language{.name = "Lua",
             .mime = kLuaMime,
             .extensions = kLuaExtensions,
             .interpreters = kLuaInterpreters,
             .fence_names = kLuaFences,
             .syntax = &kLua},
    Language{.name = "Luau",
             .extensions = kLuauExtensions,
             .interpreters = kLuauInterpreters,
             .fence_names = kLuauFences,
             .syntax = &kLua},
    Language{.name = "Shell",
             .mime = kShellMime,
             .extensions = kShellExtensions,
             .file_names = kShellNames,
             .interpreters = kShellInterpreters,
             .fence_names = kShellFences,
             .syntax = &kShell},
    Language{.name = "fish",
             .mime = kFishMime,
             .extensions = kFishExtensions,
             .interpreters = kFishInterpreters,
             .fence_names = kFishFences,
             .syntax = &kFish},
    Language{.name = "CMake",
             .mime = kCMakeMime,
             .extensions = kCMakeExtensions,
             .file_names = kCMakeNames,
             .fence_names = kCMakeFences,
             .syntax = &kCMake},
    Language{.name = "Makefile",
             .mime = kMakeMime,
             .extensions = kMakeExtensions,
             .file_names = kMakeNames,
             .interpreters = kMakeInterpreters,
             .fence_names = kMakeFences,
             .syntax = &kMake},
    Language{.name = "JSON",
             .mime = kJsonMime,
             .extensions = kJsonExtensions,
             .file_names = kJsonNames,
             .fence_names = kJsonFences,
             .syntax = &kJson},
    Language{.name = "TOML",
             .mime = kTomlMime,
             .extensions = kTomlExtensions,
             .file_names = kTomlNames,
             .fence_names = kTomlFences,
             .syntax = &kToml},
    Language{.name = "YAML",
             .mime = kYamlMime,
             .extensions = kYamlExtensions,
             .file_names = kYamlNames,
             .fence_names = kYamlFences,
             .syntax = &kYaml},
    Language{.name = "INI",
             .mime = kIniMime,
             .extensions = kIniExtensions,
             .file_names = kIniNames,
             .fence_names = kIniFences,
             .syntax = &kIni},
    Language{.name = "Ignore patterns",
             .file_names = kIgnoreNames,
             .fence_names = kIgnoreFences,
             .syntax = &kIgnore},
    Language{.name = "Git attributes",
             .file_names = kAttributesNames,
             .fence_names = kAttributesFences,
             .syntax = &kAttributes},
    Language{.name = "Markdown",
             .mime = kMarkdownMime,
             .extensions = kMarkdownExtensions,
             .syntax = &kMarkdown},
    Language{.name = "Diff",
             .mime = kDiffMime,
             .extensions = kDiffExtensions,
             .fence_names = kDiffFences,
             .syntax = &kDiff},
};

}  // namespace

std::span<const Language> all_languages() { return kLanguages; }

}  // namespace rummage::formats::text
