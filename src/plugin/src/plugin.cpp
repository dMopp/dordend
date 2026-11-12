#include "nordend/plugin/plugin.hpp"

#include <algorithm>
#include <utility>

#include "nordend/core/log.hpp"

namespace nordend::plugin {

Plugins::Plugins(Host host, const std::vector<std::string>& disabled,
                 std::span<const Builtin> available)
    : host_{std::move(host)} {
    for (const std::string& id : disabled) {
        if (std::ranges::none_of(available, [&](const Builtin& b) { return b.id == id; })) {
            log::warn("plugins: no plugin '{}' to disable (built in: {})", id, available.size());
        }
    }
    for (const Builtin& candidate : available) {
        if (std::ranges::find(disabled, candidate.id) != disabled.end()) {
            log::info("plugin {}: disabled", candidate.id);
            continue;
        }
        if (std::unique_ptr<Plugin> made = candidate.make(host_)) {
            log::info("plugin {}: running", candidate.id);
            plugins_.push_back(std::move(made));
            ids_.push_back(candidate.id);
        }
    }
}

Plugins::~Plugins() {
    while (!plugins_.empty()) {
        plugins_.pop_back();  // the last made goes first
    }
}

std::vector<std::string_view> Plugins::running() const { return ids_; }

Plugin* Plugins::for_uri(std::string_view uri) const {
    const std::optional<fs::RemoteUri> parsed = fs::parse_remote_uri(uri);
    if (!parsed) {
        return nullptr;
    }
    for (const auto& plugin : plugins_) {
        const std::vector<std::string> schemes = plugin->schemes();
        if (std::ranges::find(schemes, parsed->scheme) != schemes.end()) {
            return plugin.get();
        }
    }
    return nullptr;
}

std::vector<fs::Place> Plugins::places(std::vector<fs::Place> places) const {
    std::erase_if(places, [this](const fs::Place& place) {
        return !place.uri.empty() && !for_uri(place.uri);
    });
    for (const auto& plugin : plugins_) {
        places = plugin->places(std::move(places));
    }
    return places;
}

std::optional<views::Remotes> Plugins::remotes() {
    if (std::ranges::none_of(plugins_, [](const auto& p) { return p->can_mount(); })) {
        return std::nullopt;
    }
    return views::Remotes{
        .mount =
            [this](const std::string& uri, views::Remotes::Prompts prompts,
                   views::Remotes::Mounted done) {
                Plugin* plugin = for_uri(uri);
                if (plugin == nullptr || !plugin->can_mount()) {
                    done({}, "not available");
                    return;
                }
                plugin->mount(uri, std::move(prompts), std::move(done));
            },
        .unmount =
            [this](const std::string& uri, views::Remotes::Done done) {
                Plugin* plugin = for_uri(uri);
                if (plugin == nullptr) {
                    done("not available");
                    return;
                }
                plugin->unmount(uri, std::move(done));
            },
        .handles = [this](const std::string& uri) { return for_uri(uri) != nullptr; },
    };
}

}  // namespace nordend::plugin
