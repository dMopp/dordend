#include "rummage/platform/applications.hpp"

#include <algorithm>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "rummage/core/file.hpp"
#include "rummage/platform/exec.hpp"

namespace rummage::platform {
namespace {

std::string_view trim(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

void add_unique(std::vector<std::string>& list, const std::string& item) {
    if (std::ranges::find(list, item) == list.end()) {
        list.push_back(item);
    }
}

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::ranges::find(list, item) != list.end();
}

// The mimeapps.list files of a dir, highest priority first.
// The mimeapps.list files of a dir, highest priority first; in a data dir, also the legacy
// defaults.list and mimeinfo.cache.
void read_lists(MimeApps& lists, const std::filesystem::path& dir,
                const std::vector<std::string>& desktops, bool data,
                const fs::MimeDatabase& database) {
    for (const std::string& desktop : desktops) {
        lists.add(core::read_text_file(dir / (desktop + "-mimeapps.list")).text, &database);
    }
    lists.add(core::read_text_file(dir / "mimeapps.list").text, &database);
    if (data) {
        lists.add(core::read_text_file(dir / "defaults.list").text, &database);
        lists.add(core::read_text_file(dir / "mimeinfo.cache").text, &database);
    }
}

}  // namespace

void MimeApps::add(std::string_view text, const fs::MimeDatabase* database) {
    std::vector<std::string> Lists::* group = nullptr;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line = trim(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        if (line.starts_with('[')) {
            group = nullptr;
            if (line == "[Default Applications]") {
                group = &Lists::defaults;
            } else if (line == "[Added Associations]") {
                group = &Lists::added;
            } else if (line == "[Removed Associations]") {
                group = &Lists::removed;
            } else if (line == "[MIME Cache]") {
                group = &Lists::cached;
            }
            continue;
        }
        const std::size_t equals = line.find('=');
        if (group == nullptr || equals == std::string_view::npos) {
            continue;
        }
        std::string type{trim(line.substr(0, equals))};
        if (database != nullptr) {
            if (const fs::MimeId id = database->find(type); id != fs::kUnknownMime) {
                type = database->name(id);
            }
        }
        std::vector<std::string>& list = types[type].*group;
        for (const std::string& id : split_list(trim(line.substr(equals + 1)))) {
            add_unique(list, id);
        }
    }
}

Applications Applications::load(const Environment& environment, const fs::MimeDatabase& database) {
    Applications apps;
    apps.path_ = environment.path;
    for (const std::filesystem::path& config : environment.config_dirs) {
        Dir& dir = apps.dirs_.emplace_back();
        read_lists(dir.lists, config, environment.desktops, false, database);
    }
    for (const std::filesystem::path& data : environment.data_dirs) {
        Dir& dir = apps.dirs_.emplace_back();
        const std::filesystem::path folder = data / "applications";
        read_lists(dir.lists, folder, environment.desktops, true, database);

        std::error_code error;
        auto it = std::filesystem::recursive_directory_iterator{
            folder, std::filesystem::directory_options::skip_permission_denied, error};
        for (; !error && it != std::filesystem::recursive_directory_iterator{};
             it.increment(error)) {
            const std::filesystem::path& file = it->path();
            if (file.extension() != ".desktop" || !it->is_regular_file(error)) {
                continue;
            }
            // "kde/okular.desktop" is "kde-okular.desktop".
            std::string id = file.lexically_relative(folder).string();
            std::ranges::replace(id, '/', '-');
            if (dir.entries.contains(id)) {
                continue;
            }
            const core::TextFile text = core::read_text_file(file);
            std::optional<DesktopEntry> entry = parse_desktop_entry(text.text, environment.locale);
            if (!entry) {
                continue;
            }
            entry->id = id;
            entry->file = file;
            dir.entries.emplace(id, std::move(*entry));
        }
    }
    return apps;
}

bool Applications::masked(std::size_t index, const std::string& id) const {
    for (std::size_t i = 0; i < index; ++i) {
        if (dirs_[i].entries.contains(id)) {
            return true;
        }
    }
    return false;
}

const DesktopEntry* Applications::find(std::string_view id) const {
    const std::string key{id};
    for (const Dir& dir : dirs_) {
        const auto it = dir.entries.find(key);
        if (it == dir.entries.end()) {
            continue;
        }
        // The highest copy decides: a hidden or broken one hides the ones below.
        const DesktopEntry& entry = it->second;
        if (entry.hidden || entry.type != "Application" || entry.exec.empty()) {
            return nullptr;
        }
        if (!entry.try_exec.empty() && !find_program(entry.try_exec, path_)) {
            return nullptr;
        }
        const std::optional<std::vector<std::string>> argv = split_exec(entry.exec);
        if (!argv || !find_program(argv->front(), path_)) {
            return nullptr;
        }
        return &entry;
    }
    return nullptr;
}

std::vector<std::string> Applications::types_for(std::string_view mime,
                                                 const fs::MimeDatabase& database, bool parents) {
    // The type and its parents, breadth first, without repeats.
    std::vector<std::string> types;
    const fs::MimeId id = database.find(mime);
    types.emplace_back(id == fs::kUnknownMime ? mime : database.name(id));
    for (std::size_t i = 0; parents && i < types.size(); ++i) {
        for (const fs::MimeId parent : database.parents(database.find(types[i]))) {
            add_unique(types, std::string{database.name(parent)});
        }
    }
    return types;
}

void Applications::associate(const std::string& type, std::vector<std::string>& hits,
                             std::vector<std::string>& blocked) const {
    for (std::size_t i = 0; i < dirs_.size(); ++i) {
        const Dir& dir = dirs_[i];
        const auto consider = [&](const std::string& app, std::vector<std::string>& into) {
            if (!masked(i, app) && !contains(blocked, app) && !contains(hits, app)) {
                into.push_back(app);
            }
        };
        if (const auto it = dir.lists.types.find(type); it != dir.lists.types.end()) {
            for (const std::string& app : it->second.added) {
                consider(app, hits);
            }
            for (const std::string& app : it->second.removed) {
                consider(app, blocked);
            }
            for (const std::string& app : it->second.cached) {
                consider(app, hits);
            }
        }
    }
}

std::vector<const DesktopEntry*> Applications::all_for(std::string_view mime,
                                                       const fs::MimeDatabase& database,
                                                       bool parents) const {
    std::vector<std::string> hits;
    std::vector<std::string> blocked;  // kept from one type to the next, as GLib does
    for (const std::string& type : types_for(mime, database, parents)) {
        associate(type, hits, blocked);
    }
    std::vector<const DesktopEntry*> apps;
    for (const std::string& app : hits) {
        if (const DesktopEntry* entry = find(app)) {
            apps.push_back(entry);
        }
    }
    return apps;
}

const DesktopEntry* Applications::default_for(std::string_view mime,
                                              const fs::MimeDatabase& database) const {
    std::vector<std::string> blocked;  // kept from one type to the next, as GLib does
    for (const std::string& type : types_for(mime, database, true)) {
        std::vector<std::string> candidates;
        for (const Dir& dir : dirs_) {
            if (const auto it = dir.lists.types.find(type); it != dir.lists.types.end()) {
                for (const std::string& app : it->second.defaults) {
                    add_unique(candidates, app);
                }
            }
        }
        associate(type, candidates, blocked);
        for (const std::string& app : candidates) {
            if (const DesktopEntry* entry = find(app)) {
                return entry;
            }
        }
    }
    return nullptr;
}

std::size_t Applications::entry_count() const noexcept {
    std::size_t count = 0;
    for (const Dir& dir : dirs_) {
        count += dir.entries.size();
    }
    return count;
}

std::vector<std::string> Applications::known_types() const {
    std::unordered_set<std::string> seen;
    for (const Dir& dir : dirs_) {
        for (const auto& [type, lists] : dir.lists.types) {
            seen.insert(type);
        }
    }
    std::vector<std::string> types{seen.begin(), seen.end()};
    std::ranges::sort(types);
    return types;
}

}  // namespace rummage::platform
