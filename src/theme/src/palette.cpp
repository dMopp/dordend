#include "rummage/theme/palette.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <toml++/toml.hpp>
#include <utility>
#include <vector>

#include "rummage/core/config.hpp"
#include "rummage/core/file.hpp"

namespace rummage::theme {
namespace {

struct Role {
    std::string_view name;
    Color Palette::* member;
};

constexpr std::array kRoles{
    Role{"primary", &Palette::primary},
    Role{"on_primary", &Palette::on_primary},
    Role{"primary_container", &Palette::primary_container},
    Role{"on_primary_container", &Palette::on_primary_container},
    Role{"secondary", &Palette::secondary},
    Role{"on_secondary", &Palette::on_secondary},
    Role{"secondary_container", &Palette::secondary_container},
    Role{"on_secondary_container", &Palette::on_secondary_container},
    Role{"tertiary", &Palette::tertiary},
    Role{"on_tertiary", &Palette::on_tertiary},
    Role{"tertiary_container", &Palette::tertiary_container},
    Role{"on_tertiary_container", &Palette::on_tertiary_container},
    Role{"error", &Palette::error},
    Role{"on_error", &Palette::on_error},
    Role{"error_container", &Palette::error_container},
    Role{"on_error_container", &Palette::on_error_container},
    Role{"surface", &Palette::surface},
    Role{"on_surface", &Palette::on_surface},
    Role{"surface_variant", &Palette::surface_variant},
    Role{"on_surface_variant", &Palette::on_surface_variant},
    Role{"surface_dim", &Palette::surface_dim},
    Role{"surface_bright", &Palette::surface_bright},
    Role{"surface_container_lowest", &Palette::surface_container_lowest},
    Role{"surface_container_low", &Palette::surface_container_low},
    Role{"surface_container", &Palette::surface_container},
    Role{"surface_container_high", &Palette::surface_container_high},
    Role{"surface_container_highest", &Palette::surface_container_highest},
    Role{"outline", &Palette::outline},
    Role{"outline_variant", &Palette::outline_variant},
    Role{"shadow", &Palette::shadow},
    Role{"scrim", &Palette::scrim},
    Role{"hover", &Palette::hover},
    Role{"on_hover", &Palette::on_hover},
};

// "#rrggbb" or "#rrggbbaa", either case.
std::optional<Color> parse_hex(std::string_view text) {
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#') {
        return std::nullopt;
    }
    const std::string_view digits = text.substr(1);
    std::uint32_t value = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return digits.size() == 6 ? Color::rgb(value) : Color::rgba(value);
}

// Problems with their position, printed in file order (toml++ tables iterate by key).
class Problems {
public:
    explicit Problems(std::string_view source) : source_{source} {}

    void report(const toml::source_region& where, std::string_view what) {
        problems_.push_back({where.begin, std::format("{}:{}:{}: {}", source_, where.begin.line,
                                                      where.begin.column, what)});
    }

    [[nodiscard]] std::vector<std::string> take() {
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

private:
    struct Problem {
        toml::source_position where;
        std::string text;
    };

    std::string_view source_;
    std::vector<Problem> problems_;
};

void read_colors(const toml::node& node, Problems& problems, Palette& palette) {
    const toml::table* table = node.as_table();
    if (table == nullptr) {
        problems.report(node.source(), "'colors' must be a section ([colors]); using defaults");
        return;
    }
    std::string missing;
    for (const Role& role : kRoles) {
        const toml::node* value = table->get(role.name);
        if (value == nullptr) {
            missing += missing.empty() ? "" : ", ";
            missing += role.name;
            continue;
        }
        const auto* text = value->as_string();
        const std::optional<Color> color = text != nullptr ? parse_hex(text->get()) : std::nullopt;
        if (!color) {
            problems.report(value->source(),
                            std::format("'colors.{}' must be \"#rrggbb\" or \"#rrggbbaa\"; using "
                                        "the default",
                                        role.name));
            continue;
        }
        palette.*role.member = *color;
    }
    // Other keys are fine: the template writes every Noctalia token, rummage uses some.
    if (!missing.empty()) {
        problems.report(node.source(),
                        std::format("missing colors, defaults used for: {}", missing));
    }
}

}  // namespace

std::filesystem::path default_colors_path() {
    const std::filesystem::path config = core::default_config_path();
    return config.empty() ? config : config.parent_path() / "colors.toml";
}

PaletteResult parse_palette(std::string_view text, std::string_view source) {
    PaletteResult result;
    Problems problems{source};
    try {
        const toml::table root = toml::parse(text, source);
        result.from_file = true;
        bool has_colors = false;
        for (const auto& [key, node] : root) {
            if (key.str() == "colors") {
                has_colors = true;
                read_colors(node, problems, result.palette);
            } else if (key.str() == "mode") {
                const std::optional<std::string_view> mode = node.value<std::string_view>();
                if (mode == "dark" || mode == "light") {
                    result.palette.dark = mode == "dark";
                } else {
                    problems.report(node.source(),
                                    R"('mode' must be "dark" or "light"; using "dark")");
                }
            } else {
                problems.report(key.source(), std::format("unknown key '{}'; ignored", key.str()));
            }
        }
        if (!has_colors) {
            problems.report(root.source(), "no [colors] section; using the default palette");
        }
    } catch (const toml::parse_error& e) {
        result.from_file = false;
        problems.report(e.source(), std::format("{}; the whole file is ignored", e.description()));
    }
    result.problems = problems.take();
    return result;
}

PaletteResult load_palette(const std::filesystem::path& path) {
    const core::TextFile file = core::read_text_file(path);
    if (!file.exists) {
        return {};
    }
    if (!file.error.empty()) {
        PaletteResult result;
        result.problems.push_back(std::format("{}: {}", path.string(), file.error));
        return result;
    }
    return parse_palette(file.text, path.string());
}

}  // namespace rummage::theme
