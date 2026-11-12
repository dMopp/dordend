#include "nordend/core/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

#include "config_template.hpp"
#include "nordend/core/file.hpp"

namespace nordend::core {
namespace {

const char* type_name(const toml::node& node) {
    switch (node.type()) {
        case toml::node_type::string:
            return "a string";
        case toml::node_type::integer:
            return "an integer";
        case toml::node_type::floating_point:
            return "a number with decimals";
        case toml::node_type::boolean:
            return "a boolean";
        case toml::node_type::date:
            return "a date";
        case toml::node_type::time:
            return "a time";
        case toml::node_type::date_time:
            return "a date-time";
        case toml::node_type::array:
            return "an array";
        case toml::node_type::table:
            return "a table";
        case toml::node_type::none:
            break;
    }
    return "nothing";
}

// Walks the parsed document, filling the config and collecting problems.
class Reader {
public:
    // A key inside a section and how to read its value.
    struct Field {
        std::string_view name;
        std::function<void(const toml::node& node, const std::string& path)> read;
    };

    explicit Reader(std::string_view source) : source_{source} {}

    void report(const toml::source_region& where, std::string_view what) {
        problems_.push_back({where.begin, std::format("{}:{}:{}: {}", source_, where.begin.line,
                                                      where.begin.column, what)});
    }

    // Problems in file order (toml++ tables iterate by key, not by position).
    [[nodiscard]] std::vector<std::string> take_problems() {
        std::ranges::stable_sort(problems_, [](const Problem& a, const Problem& b) {
            return std::pair{a.where.line, a.where.column} <
                   std::pair{b.where.line, b.where.column};
        });
        std::vector<std::string> out;
        out.reserve(problems_.size());
        for (Problem& problem : problems_) {
            out.push_back(std::move(problem.text));
        }
        problems_.clear();
        return out;
    }

    void read_section(const toml::node& node, std::string_view section,
                      std::initializer_list<Field> fields) {
        const toml::table* table = node.as_table();
        if (table == nullptr) {
            report(node.source(), std::format("'{}' must be a section ([{}]), got {}; ignored",
                                              section, section, type_name(node)));
            return;
        }
        for (const auto& [key, value] : *table) {
            const std::string path = std::format("{}.{}", section, key.str());
            bool known = false;
            for (const Field& field : fields) {
                if (key.str() == field.name) {
                    field.read(value, path);
                    known = true;
                    break;
                }
            }
            if (!known) {
                report(key.source(), std::format("unknown key '{}'; ignored", path));
            }
        }
    }

    void read_string(const toml::node& node, const std::string& path, std::string& out) {
        const auto* value = node.as_string();
        if (value == nullptr) {
            report(node.source(), std::format("'{}' must be a string, got {}; using \"{}\"", path,
                                              type_name(node), out));
            return;
        }
        if (value->get().empty()) {
            report(node.source(), std::format("'{}' must not be empty; using \"{}\"", path, out));
            return;
        }
        out = value->get();
    }

    void read_bool(const toml::node& node, const std::string& path, bool& out) {
        const auto* value = node.as_boolean();
        if (value == nullptr) {
            report(node.source(), std::format("'{}' must be true or false, got {}; using {}", path,
                                              type_name(node), out));
            return;
        }
        out = value->get();
    }

    void read_string_list(const toml::node& node, const std::string& path,
                          std::vector<std::string>& out) {
        const auto* array = node.as_array();
        if (array == nullptr) {
            report(node.source(), std::format("'{}' must be a list of strings, got {}; ignored",
                                              path, type_name(node)));
            return;
        }
        std::vector<std::string> values;
        for (const toml::node& item : *array) {
            const auto* value = item.as_string();
            if (value == nullptr || value->get().empty()) {
                report(item.source(),
                       std::format("'{}' must hold names (strings); this one is ignored", path));
                continue;
            }
            values.push_back(value->get());
        }
        out = std::move(values);
    }

    void read_opacity(const toml::node& node, const std::string& path, float& out) {
        const auto value = node.value<double>();
        if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 1.0) {
            report(node.source(),
                   std::format("'{}' must be a number between 0 and 1; using {}", path, out));
            return;
        }
        out = static_cast<float>(*value);
    }

    void read_int(const toml::node& node, const std::string& path, int min, int max, int& out) {
        const auto* value = node.as_integer();
        if (value == nullptr) {
            report(node.source(), std::format("'{}' must be an integer, got {}; using {}", path,
                                              type_name(node), out));
            return;
        }
        const std::int64_t number = value->get();
        if (number < min || number > max) {
            report(node.source(), std::format("'{}' must be between {} and {}, got {}; using {}",
                                              path, min, max, number, out));
            return;
        }
        out = static_cast<int>(number);
    }

private:
    struct Problem {
        toml::source_position where;
        std::string text;
    };

    std::string_view source_;
    std::vector<Problem> problems_;
};

void read_document(const toml::table& root, Reader& reader, Config& config) {
    for (const auto& [key, node] : root) {
        if (key.str() == "appearance") {
            reader.read_section(
                node, "appearance",
                {{"background_opacity", [&](const toml::node& value, const std::string& path) {
                      reader.read_opacity(value, path, config.appearance.background_opacity);
                  }}});
        } else if (key.str() == "keybindings") {
            if (const auto* table = node.as_table()) {
                for (const auto& [name, value] : *table) {
                    if (const auto* chord = value.as_string()) {
                        config.keybindings.emplace(name.str(), chord->get());
                    } else {
                        reader.report(
                            value.source(),
                            std::format("keybindings.{} must be a string; ignored", name.str()));
                    }
                }
            } else {
                reader.report(node.source(), "keybindings must be a section; ignored");
            }
        } else if (key.str() == "font") {
            reader.read_section(
                node, "font",
                {{"family",
                  [&](const toml::node& value, const std::string& path) {
                      reader.read_string(value, path, config.font.family);
                  }},
                 {"monospace", [&](const toml::node& value, const std::string& path) {
                      reader.read_string(value, path, config.font.monospace);
                  }}});
        } else if (key.str() == "pointer") {
            reader.read_section(
                node, "pointer",
                {{"double_click_ms",
                  [&](const toml::node& value, const std::string& path) {
                      reader.read_int(value, path, 100, 2000, config.pointer.double_click_ms);
                  }},
                 {"double_click_distance", [&](const toml::node& value, const std::string& path) {
                      reader.read_int(value, path, 0, 50, config.pointer.double_click_distance);
                  }}});
        } else if (key.str() == "icon") {
            reader.read_section(
                node, "icon",
                {{"theme",
                  [&](const toml::node& value, const std::string& path) {
                      reader.read_string(value, path, config.icon.theme);
                  }},
                 {"follow_accent", [&](const toml::node& value, const std::string& path) {
                      reader.read_bool(value, path, config.icon.follow_accent);
                  }}});
        } else if (key.str() == "plugins") {
            reader.read_section(
                node, "plugins",
                {{"disabled", [&](const toml::node& value, const std::string& path) {
                      reader.read_string_list(value, path, config.plugins.disabled);
                  }}});
        } else {
            reader.report(key.source(),
                          std::format("unknown section or key '{}'; ignored", key.str()));
        }
    }
}

}  // namespace

std::filesystem::path default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");  // NOLINT(concurrency-mt-unsafe)
    // The XDG base directory spec ignores relative paths.
    if (xdg != nullptr && xdg[0] == '/') {
        return std::filesystem::path{xdg} / "nordend" / "config.toml";
    }
    const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".config" / "nordend" / "config.toml";
    }
    return {};
}

std::error_code create_default_config(const std::filesystem::path& path) {
    if (path.empty()) {
        return {};
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() != std::filesystem::file_type::not_found) {
        return error;
    }
    return write_file_atomically(path, kDefaultConfig, false);
}

ConfigResult parse_config(std::string_view text, std::string_view source) {
    ConfigResult result;
    Reader reader{source};
    toml::table root;
    try {
        root = toml::parse(text, source);
        result.from_file = true;
        read_document(root, reader, result.config);
    } catch (const toml::parse_error& e) {
        reader.report(e.source(), std::format("{}; the whole file is ignored, using defaults",
                                              e.description()));
    }
    result.problems = reader.take_problems();
    return result;
}

ConfigResult load_config(const std::filesystem::path& path) {
    const TextFile file = read_text_file(path);
    if (!file.exists) {
        return {};
    }
    if (!file.error.empty()) {
        ConfigResult result;
        result.problems.push_back(std::format("{}: {}; using defaults", path.string(), file.error));
        return result;
    }
    return parse_config(file.text, path.string());
}

}  // namespace nordend::core
