#include "rummage/core/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <initializer_list>
#include <string>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <utility>
#include <vector>

#include "config_template.hpp"
#include "rummage/core/file.hpp"

namespace rummage::core {
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

    void read_text_size(const toml::node& node, const std::string& path, float& out) {
        const auto value = node.value<double>();
        if (!value || !std::isfinite(*value) || *value < 8.0 || *value > 32.0) {
            report(node.source(),
                   std::format("'{}' must be a number between 8 and 32; using {}", path, out));
            return;
        }
        out = static_cast<float>(*value);
    }

private:
    struct Problem {
        toml::source_position where;
        std::string text;
    };

    std::string_view source_;
    std::vector<Problem> problems_;
};

bool read_actions(const toml::node& node, Reader& reader, std::vector<Action>& actions) {
    const auto* array = node.as_array();
    if (array == nullptr) {
        reader.report(node.source(), "actions must be an array of tables ([[actions]])");
        return false;
    }
    bool valid = true;
    for (const auto& entry : *array) {
        const auto fail = [&](std::string_view message) {
            reader.report(entry.source(), std::format("actions: {}; action set ignored", message));
            valid = false;
        };
        const auto* table = entry.as_table();
        if (table == nullptr) {
            fail("each action must be a table");
            continue;
        }
        Action action;
        if (const auto id = (*table)["id"].value<std::string>()) {
            const auto defaults = action_registry();
            const auto preset = std::ranges::find(defaults, *id, &Action::id);
            if (preset != defaults.end() && preset->kind != Action::Kind::copy_location) {
                action = *preset;
            }
        }
        for (const auto& [key, value] : *table) {
            const auto name = key.str();
            if (name == "id" || name == "label" || name == "selection" || name == "execution" ||
                name == "placement" || name == "destination" || name == "destination_name") {
                const auto text = value.value<std::string>();
                if (!text) {
                    fail(std::format("{} must be a string", name));
                } else if (name == "id") {
                    action.id = *text;
                } else if (name == "label") {
                    action.label = *text;
                } else if (name == "destination_name") {
                    action.destination_name = *text;
                } else if (name == "destination") {
                    if (*text == "none") {
                        action.destination = Action::Destination::none;
                    } else if (*text == "file") {
                        action.destination = Action::Destination::file;
                    } else if (*text == "directory") {
                        action.destination = Action::Destination::directory;
                    } else {
                        fail("destination must be none, file or directory");
                    }
                } else if (name == "placement") {
                    if (*text == "actions") {
                        action.placement = Action::Placement::actions;
                    } else if (*text == "menu") {
                        action.placement = Action::Placement::menu;
                    } else {
                        fail("placement must be actions or menu");
                    }
                } else if (name == "execution") {
                    if (*text != "handoff") {
                        fail("only execution = 'handoff' is currently supported");
                    }
                } else if (*text == "one") {
                    action.selection = Action::Selection::one;
                } else if (*text == "many") {
                    action.selection = Action::Selection::many;
                } else if (*text == "background") {
                    action.selection = Action::Selection::background;
                } else if (*text == "any") {
                    action.selection = Action::Selection::any;
                } else {
                    fail("selection must be one, many, background or any");
                }
            } else if (name == "argv" || name == "mime") {
                auto& output = name == "argv" ? action.argv : action.mime;
                output.clear();
                if (name == "argv") {
                    action.kind = Action::Kind::external;
                }
                if (const auto* values = value.as_array()) {
                    for (const auto& item : *values) {
                        if (const auto text = item.value<std::string>()) {
                            output.push_back(*text);
                        } else {
                            fail(std::format("{} must contain only strings", name));
                        }
                    }
                } else {
                    fail(std::format("{} must be an array", name));
                }
            } else if (name == "notify_on_completion") {
                if (const auto* enabled = value.as_boolean()) {
                    action.notify_on_completion = enabled->get();
                } else {
                    fail("notify_on_completion must be a boolean");
                }
            } else if (name == "enabled") {
                if (const auto enabled = value.value<bool>()) {
                    action.enabled = *enabled;
                } else {
                    fail("enabled must be a boolean");
                }
            } else {
                fail(std::format("unknown key '{}'", name));
            }
        }
        if (const auto problem = action_problem(action)) {
            fail(*problem);
        }
        if (std::ranges::find(actions, action.id, &Action::id) != actions.end()) {
            fail(std::format("duplicate id '{}'", action.id));
        }
        actions.push_back(std::move(action));
    }
    if (!valid) {
        actions.clear();
    }
    return valid;
}

void read_document(const toml::table& root, Reader& reader, ConfigResult& result) {
    Config& config = result.config;
    for (const auto& [key, node] : root) {
        if (key.str() == "actions") {
            result.actions_valid = read_actions(node, reader, config.actions);
        } else if (key.str() == "appearance") {
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
        } else if (key.str() == "status_bar") {
            reader.read_section(
                node, "status_bar",
                {{"text_size", [&](const toml::node& value, const std::string& path) {
                      reader.read_text_size(value, path, config.status_bar.text_size);
                  }}});
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
                 {"places",
                  [&](const toml::node& value, const std::string& path) {
                      const auto* table = value.as_table();
                      if (!table) {
                          reader.report(value.source(), path + " must be a section; ignored");
                          return;
                      }
                      for (const auto& [name, entry] : *table) {
                          static constexpr std::string_view keys[]{
                              "home",     "desktop", "documents",  "downloads", "music",
                              "pictures", "videos",  "filesystem", "trash",     "trash_full"};
                          const std::string entry_path = path + "." + std::string{name.str()};
                          if (std::ranges::find(keys, name.str()) == std::end(keys)) {
                              reader.report(name.source(),
                                            "unknown key '" + entry_path + "'; ignored");
                              continue;
                          }
                          std::string icon;
                          reader.read_string(entry, entry_path, icon);
                          if (!icon.empty()) {
                              config.icon.places.emplace(name.str(), std::move(icon));
                          }
                      }
                  }},
                 {"follow_accent", [&](const toml::node& value, const std::string& path) {
                      reader.read_bool(value, path, config.icon.follow_accent);
                  }}});
        } else if (key.str() == "context_menu") {
            // Read after actions so references are independent of TOML order.
        } else {
            reader.report(key.source(),
                          std::format("unknown section or key '{}'; ignored", key.str()));
        }
    }
}

bool read_menu_layout(const toml::node& source, Reader& reader, const std::vector<Action>& actions,
                      MenuLayout& layout) {
    bool valid = true;
    const auto fail = [&](const toml::node& node, std::string message) {
        reader.report(node.source(), "context_menu: " + message + "; layout ignored");
        valid = false;
    };
    const auto* table = source.as_table();
    if (!table) {
        fail(source, "must be a table");
        return false;
    }
    std::unordered_set<std::string> known{"open",
                                          "properties",
                                          "open_with",
                                          "open_tab",
                                          "open_terminal",
                                          "add_bookmark",
                                          "copy",
                                          "cut",
                                          "paste",
                                          "rename",
                                          "trash",
                                          "erase",
                                          "restore",
                                          "empty_trash",
                                          "new_folder",
                                          "new_file",
                                          "empty_file",
                                          "sort",
                                          "sort:name",
                                          "sort:size",
                                          "sort:modified",
                                          "sort:type",
                                          "sort:ascending",
                                          "sort:descending",
                                          "actions",
                                          "mount",
                                          "unmount",
                                          "eject",
                                          "remove_bookmark",
                                          "panes",
                                          "toggle_split",
                                          "switch_pane",
                                          "swap_panes",
                                          "orient_panes",
                                          "open_other_pane",
                                          "same_other_pane",
                                          "copy_other_pane",
                                          "move_other_pane"};
    for (const auto& action : action_registry(actions)) {
        known.insert("action:" + action.id);
    }
    for (const auto& [key, value] : *table) {
        std::optional<std::vector<MenuNode>>* output = nullptr;
        if (key.str() == "items")
            output = &layout.items;
        else if (key.str() == "entry")
            output = &layout.entry;
        else if (key.str() == "background")
            output = &layout.background;
        else if (key.str() == "trash")
            output = &layout.trash;
        else if (key.str() == "sidebar")
            output = &layout.sidebar;
        else {
            fail(value, std::format("unknown layout '{}'", key.str()));
            continue;
        }
        std::unordered_set<std::string> used;
        bool remaining = false;
        const auto read = [&](const auto& self, const toml::node& node,
                              std::vector<MenuNode>& nodes) -> void {
            const auto* array = node.as_array();
            if (!array) {
                fail(node, "items must be an array of tables");
                return;
            }
            for (const auto& entry : *array) {
                const auto* fields = entry.as_table();
                if (!fields) {
                    fail(entry, "each item must be a table");
                    continue;
                }
                MenuNode item;
                bool has_items = false;
                for (const auto& [name, field] : *fields) {
                    if (name.str() == "id" || name.str() == "label") {
                        const auto text = field.value<std::string>();
                        if (!text || text->empty() || text->find('\0') != std::string::npos) {
                            fail(field, std::format("{} must be a nonempty string", name.str()));
                        } else if (name.str() == "id") {
                            item.id = *text;
                        } else {
                            item.label = *text;
                        }
                    } else if (name.str() == "items") {
                        has_items = true;
                        self(self, field, item.items);
                    } else if (name.str() == "separator" || name.str() == "remaining") {
                        if (const auto flag = field.value_exact<bool>()) {
                            (name.str() == "separator" ? item.separator : item.remaining) = *flag;
                        } else {
                            fail(field, std::format("{} must be a boolean", name.str()));
                        }
                    } else {
                        fail(field, std::format("unknown item key '{}'", name.str()));
                    }
                }
                if (!item.id.empty()) {
                    if (has_items || item.remaining) {
                        fail(entry, "id cannot be combined with items or remaining");
                    }
                    if (!known.contains(item.id)) {
                        fail(entry, "unknown item id '" + item.id + "'");
                    }
                    if (!used.insert(item.id).second) {
                        fail(entry, "duplicate item id '" + item.id + "'");
                    }
                } else if (item.remaining) {
                    if (has_items || !item.label.empty() || remaining) {
                        fail(entry, "remaining must stand alone and occur only once per layout");
                    }
                    remaining = true;
                } else if (has_items) {
                    if (item.label.empty())
                        fail(entry, "a group requires a label");
                } else if (!item.label.empty() || !item.separator) {
                    fail(entry, "expected an id, a labeled group with items, or a separator");
                }
                nodes.push_back(std::move(item));
            }
        };
        output->emplace();
        read(read, value, **output);
    }
    if (!valid)
        layout = {};
    return valid;
}

}  // namespace

std::filesystem::path default_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");  // NOLINT(concurrency-mt-unsafe)
    // The XDG base directory spec ignores relative paths.
    if (xdg != nullptr && xdg[0] == '/') {
        return std::filesystem::path{xdg} / "rummage" / "config.toml";
    }
    const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".config" / "rummage" / "config.toml";
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
        read_document(root, reader, result);
        if (const auto* menu = root.get("context_menu")) {
            result.context_menu_valid =
                read_menu_layout(*menu, reader, result.config.actions, result.config.context_menu);
        }
    } catch (const toml::parse_error& e) {
        result.actions_valid = false;
        result.context_menu_valid = false;
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

}  // namespace rummage::core
