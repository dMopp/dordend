#include <algorithm>
#include <format>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>

#include "rummage/engines/engines.hpp"

namespace rummage::engines {
namespace {

// A manifest that is not one: what is wrong, and where.
struct Invalid : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view source, const toml::node& node, std::string_view what) {
    throw Invalid{std::format("{}:{}: {}", source, node.source().begin.line, what)};
}

bool is_name(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

// A table's keys must all be known: a misspelt one would otherwise be silently ignored.
void known_keys(std::string_view source, const toml::table& table, std::string_view where,
                std::initializer_list<std::string_view> keys) {
    for (const auto& [key, node] : table) {
        if (std::ranges::find(keys, key.str()) == keys.end()) {
            fail(source, node, std::format("unknown key '{}' in {}", key.str(), where));
        }
    }
}

const toml::node& required(std::string_view source, const toml::table& table,
                           std::string_view where, std::string_view key) {
    const toml::node* node = table.get(key);
    if (node == nullptr) {
        fail(source, table, std::format("{} has no '{}'", where, key));
    }
    return *node;
}

std::string text(std::string_view source, const toml::node& node, std::string_view key) {
    const auto* value = node.as_string();
    if (value == nullptr) {
        fail(source, node, std::format("'{}' must be a string", key));
    }
    return value->get();
}

bool flag(std::string_view source, const toml::table& table, std::string_view key) {
    const toml::node* node = table.get(key);
    if (node == nullptr) {
        return false;
    }
    const auto* value = node->as_boolean();
    if (value == nullptr) {
        fail(source, *node, std::format("'{}' must be true or false", key));
    }
    return value->get();
}

Family read_family(std::string_view source, const toml::table& table) {
    known_keys(source, table, "[[family]]",
               {"name", "shows", "info", "thumbnails", "subclasses", "mime"});
    Family family;
    family.name = text(source, required(source, table, "[[family]]", "name"), "name");
    if (!is_name(family.name)) {
        fail(source, table, std::format("family name '{}' is not [a-z0-9-]", family.name));
    }
    const toml::node& shows = required(source, table, "[[family]]", "shows");
    const std::string what = text(source, shows, "shows");
    if (what == "picture") {
        family.shows = Shows::picture;
    } else if (what == "text") {
        family.shows = Shows::text;
    } else {
        fail(source, shows, R"('shows' must be "picture" or "text")");
    }
    family.info = flag(source, table, "info");
    family.thumbnails = flag(source, table, "thumbnails");
    family.subclasses = flag(source, table, "subclasses");
    const toml::node& mime = required(source, table, "[[family]]", "mime");
    const auto* types = mime.as_array();
    if (types == nullptr || types->empty()) {
        fail(source, mime, "'mime' must be a list of types, not empty");
    }
    for (const toml::node& type : *types) {
        std::string name = text(source, type, "mime");
        const std::size_t slash = name.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 == name.size()) {
            fail(source, type, std::format("'{}' is not a MIME type", name));
        }
        if (std::ranges::find(family.mime, name) != family.mime.end()) {
            fail(source, type, std::format("'{}' is listed twice", name));
        }
        family.mime.push_back(std::move(name));
    }
    return family;
}

Manifest read_manifest(std::string_view source, const toml::table& root) {
    known_keys(source, root, "the manifest", {"engine", "family"});
    const auto* engine = required(source, root, "the manifest", "engine").as_table();
    if (engine == nullptr) {
        fail(source, root, "[engine] must be a table");
    }
    known_keys(source, *engine, "[engine]", {"name", "program", "protocol"});

    const toml::node& protocol = required(source, *engine, "[engine]", "protocol");
    const auto* number = protocol.as_integer();
    if (number == nullptr) {
        fail(source, protocol, "'protocol' must be a whole number");
    }
    if (number->get() != kProtocol) {
        fail(source, protocol,
             std::format("protocol {}, rummage speaks {}", number->get(), kProtocol));
    }

    Manifest manifest;
    manifest.name = text(source, required(source, *engine, "[engine]", "name"), "name");
    if (!is_name(manifest.name)) {
        fail(source, *engine, std::format("engine name '{}' is not [a-z0-9-]", manifest.name));
    }
    manifest.program = text(source, required(source, *engine, "[engine]", "program"), "program");
    if (manifest.program.empty() || manifest.program == "." || manifest.program == ".." ||
        manifest.program.find('/') != std::string::npos) {
        fail(source, *engine,
             std::format("program '{}' must be a file name, not a path", manifest.program));
    }

    const toml::node& families = required(source, root, "the manifest", "family");
    const auto* list = families.as_array();
    if (list == nullptr || list->empty() || !list->is_array_of_tables()) {
        fail(source, families, "'family' must be one or more [[family]] tables");
    }
    for (const toml::node& family : *list) {
        Family read = read_family(source, *family.as_table());
        for (const Family& other : manifest.families) {
            if (other.name == read.name) {
                fail(source, family, std::format("family '{}' is listed twice", read.name));
            }
            for (const std::string& type : read.mime) {
                if (std::ranges::find(other.mime, type) != other.mime.end()) {
                    fail(source, family,
                         std::format("'{}' is in families '{}' and '{}'", type, other.name,
                                     read.name));
                }
            }
        }
        manifest.families.push_back(std::move(read));
    }
    return manifest;
}

}  // namespace

std::optional<Manifest> parse_manifest(std::string_view text, std::string_view source,
                                       std::string& why) {
    try {
        const toml::table root = toml::parse(text, source);
        return read_manifest(source, root);
    } catch (const toml::parse_error& error) {
        why = std::format("{}:{}: {}", source, error.source().begin.line, error.description());
    } catch (const Invalid& invalid) {
        why = invalid.what();
    }
    return std::nullopt;
}

}  // namespace rummage::engines
