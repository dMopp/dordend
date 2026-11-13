#include "rummage/platform/exec.hpp"

#include <array>
#include <utility>

#include "rummage/thumbs/cache.hpp"

namespace rummage::platform {
namespace {

// How a terminal runs a command: its program and the arguments before the command.
struct Terminal {
    std::string_view program;
    std::array<std::string_view, 2> before;  // empty strings are skipped
};

// Known terminals, in the order they are tried. The forms of the ones not installed on the
// development machine are taken from their manuals (to be verified on a machine that has them).
constexpr std::array<Terminal, 8> kTerminals{{
    {.program = "foot", .before = {"--", ""}},
    {.program = "ghostty", .before = {"-e", ""}},
    {.program = "kitty", .before = {"", ""}},  // stops at the first word that is not an option
    {.program = "alacritty", .before = {"-e", ""}},
    {.program = "wezterm", .before = {"start", "--"}},
    {.program = "konsole", .before = {"-e", ""}},
    {.program = "gnome-terminal", .before = {"--", ""}},
    {.program = "xterm", .before = {"-e", ""}},
}};

std::vector<std::string> terminal_line(std::string program, const Terminal* known,
                                       const std::vector<std::string>& argv) {
    std::vector<std::string> line{std::move(program)};
    if (known != nullptr) {
        for (const std::string_view arg : known->before) {
            if (!arg.empty()) {
                line.emplace_back(arg);
            }
        }
    } else {
        line.emplace_back("-e");  // the convention most terminals follow
    }
    line.insert(line.end(), argv.begin(), argv.end());
    return line;
}

const Terminal* known_terminal(std::string_view program) {
    const std::string_view name = program.substr(program.rfind('/') + 1);
    for (const Terminal& terminal : kTerminals) {
        if (terminal.program == name) {
            return &terminal;
        }
    }
    return nullptr;
}

// The field code an argument is made of, or 0 when it is not exactly one ("%F", "%U", "%i").
char whole_code(const std::string& arg) { return arg.size() == 2 && arg[0] == '%' ? arg[1] : '\0'; }

}  // namespace

std::optional<std::vector<std::string>> split_exec(std::string_view exec) {
    // A subset of shell quoting, as GLib's g_shell_parse_argv reads it: the spec's double quotes,
    // and also single quotes and backslashes outside quotes, which real desktop files use.
    std::vector<std::string> args;
    std::string arg;
    bool in_arg = false;
    for (std::size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (c == ' ' || c == '\t' || c == '\n') {
            if (in_arg) {
                args.push_back(std::move(arg));
                arg.clear();
                in_arg = false;
            }
            continue;
        }
        in_arg = true;
        if (c == '"') {
            for (++i;; ++i) {
                if (i == exec.size()) {
                    return std::nullopt;  // not closed
                }
                if (exec[i] == '"') {
                    break;
                }
                if (exec[i] == '\\' && i + 1 < exec.size() &&
                    std::string_view{"\"`$\\"}.find(exec[i + 1]) != std::string_view::npos) {
                    ++i;
                }
                arg.push_back(exec[i]);
            }
        } else if (c == '\'') {
            const std::size_t end = exec.find('\'', i + 1);
            if (end == std::string_view::npos) {
                return std::nullopt;
            }
            arg += exec.substr(i + 1, end - i - 1);
            i = end;
        } else if (c == '\\' && i + 1 < exec.size()) {
            arg.push_back(exec[++i]);
        } else {
            arg.push_back(c);
        }
    }
    if (in_arg) {
        args.push_back(std::move(arg));
    }
    if (args.empty()) {
        return std::nullopt;
    }
    return args;
}

std::vector<std::vector<std::string>> expand_exec(const DesktopEntry& entry,
                                                  std::span<const std::filesystem::path> files) {
    const std::optional<std::vector<std::string>> args = split_exec(entry.exec);
    if (!args) {
        return {};
    }
    bool takes_list = false;
    bool takes_one = false;
    for (const std::string& arg : *args) {
        for (std::size_t i = 0; i + 1 < arg.size(); ++i) {
            if (arg[i] == '%') {
                const char code = arg[++i];
                takes_list = takes_list || code == 'F' || code == 'U';
                takes_one = takes_one || code == 'f' || code == 'u';
            }
        }
    }

    // The command line for some files: all of them with a list code, one otherwise.
    const auto expand = [&](std::span<const std::filesystem::path> given) {
        std::vector<std::string> line;
        for (const std::string& arg : *args) {
            const char whole = whole_code(arg);
            if (whole == 'F' || whole == 'U') {
                for (const std::filesystem::path& file : given) {
                    line.push_back(whole == 'F' ? file.string() : thumbs::file_uri(file));
                }
                continue;
            }
            if (whole == 'i') {
                if (!entry.icon.empty()) {
                    line.emplace_back("--icon");
                    line.push_back(entry.icon);
                }
                continue;
            }
            std::string out;
            bool coded = false;
            for (std::size_t i = 0; i < arg.size(); ++i) {
                if (arg[i] != '%' || i + 1 == arg.size()) {
                    out.push_back(arg[i]);
                    continue;
                }
                coded = true;
                switch (arg[++i]) {
                    case 'f':
                    case 'F':  // inside a word: the files, separated by spaces
                    case 'u':
                    case 'U': {
                        const bool uri = arg[i] == 'u' || arg[i] == 'U';
                        for (std::size_t f = 0; f < given.size(); ++f) {
                            out += f == 0 ? "" : " ";
                            out += uri ? thumbs::file_uri(given[f]) : given[f].string();
                        }
                        break;
                    }
                    case 'c':
                        out += entry.name;
                        break;
                    case 'k':
                        out += entry.file.string();
                        break;
                    case '%':
                        out.push_back('%');
                        break;
                    default:  // deprecated (%d %D %n %N %v %m) or unknown: removed
                        break;
                }
            }
            if (!out.empty() || !coded) {  // codes that expand to nothing leave no argument
                line.push_back(std::move(out));
            }
        }
        if (!takes_list && !takes_one) {
            for (const std::filesystem::path& file : given) {
                line.push_back(file.string());
            }
        }
        return line;
    };

    std::vector<std::vector<std::string>> lines;
    if (takes_list || files.empty()) {
        lines.push_back(expand(files));
    } else {
        for (std::size_t i = 0; i < files.size(); ++i) {
            lines.push_back(expand(files.subspan(i, 1)));
        }
    }
    return lines;
}

namespace {

// The terminal to use: its program, the options $TERMINAL gave it, and its known form if any.
// is_exec: xdg-terminal-exec, which takes the command as it is.
struct FoundTerminal {
    std::string program;
    std::vector<std::string> options;
    const Terminal* known{nullptr};
    bool is_exec{false};
};

std::optional<FoundTerminal> find_terminal(const Environment& environment) {
    if (!environment.terminal.empty()) {
        if (const std::optional<std::vector<std::string>> words =
                split_exec(environment.terminal)) {
            // $TERMINAL may carry its own options ("foot --server").
            if (const std::optional<std::filesystem::path> program =
                    find_program(words->front(), environment.path)) {
                return FoundTerminal{.program = program->string(),
                                     .options = {words->begin() + 1, words->end()},
                                     .known = known_terminal(words->front()),
                                     .is_exec = false};
            }
        }
    }
    if (const std::optional<std::filesystem::path> program =
            find_program("xdg-terminal-exec", environment.path)) {
        return FoundTerminal{
            .program = program->string(), .options = {}, .known = nullptr, .is_exec = true};
    }
    for (const Terminal& terminal : kTerminals) {
        if (const std::optional<std::filesystem::path> program =
                find_program(terminal.program, environment.path)) {
            return FoundTerminal{.program = program->string(), .options = {}, .known = &terminal};
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::vector<std::string>> in_terminal(const std::vector<std::string>& argv,
                                                    const Environment& environment) {
    std::optional<FoundTerminal> found = find_terminal(environment);
    if (!found) {
        return std::nullopt;
    }
    if (found->is_exec) {
        std::vector<std::string> line{std::move(found->program)};
        line.insert(line.end(), argv.begin(), argv.end());
        return line;
    }
    // The command goes after $TERMINAL's own options.
    std::vector<std::string> line = terminal_line(std::move(found->program), found->known, argv);
    line.insert(line.begin() + 1, found->options.begin(), found->options.end());
    return line;
}

std::optional<std::vector<std::string>> terminal_alone(const Environment& environment) {
    std::optional<FoundTerminal> found = find_terminal(environment);
    if (!found) {
        return std::nullopt;
    }
    std::vector<std::string> line{std::move(found->program)};
    line.insert(line.end(), found->options.begin(), found->options.end());
    return line;
}

}  // namespace rummage::platform
