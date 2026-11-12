// plugin::Plugins: which plugins run, the places they claim, and mounting by scheme. With fake
// plugins, so it does not depend on which ones this build compiled in.

#include "nordend/plugin/plugin.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/config.hpp"
#include "nordend/core/event_loop.hpp"
#include "nordend/fs/places.hpp"

using nordend::core::EventLoop;
using nordend::fs::Place;
using nordend::plugin::Builtin;
using nordend::plugin::Host;
using nordend::plugin::Plugin;
using nordend::plugin::Plugins;
using nordend::views::Remotes;

namespace {

// What the fakes saw.
std::vector<std::string> g_mounted;

class FakeShares final : public Plugin {
public:
    [[nodiscard]] std::vector<std::string> schemes() const override { return {"smb"}; }

    [[nodiscard]] bool can_mount() const override { return true; }

    void mount(const std::string& uri, Remotes::Prompts /*prompts*/,
               Remotes::Mounted done) override {
        g_mounted.push_back(uri);
        done("/run/fake/share", {});
    }

    // A mounted share: its folder and eject icon.
    [[nodiscard]] std::vector<Place> places(std::vector<Place> places) const override {
        for (Place& place : places) {
            if (place.uri == "smb://nas/public") {
                place.path = "/run/fake/share";
                place.disk.mounted = true;
            }
        }
        return places;
    }
};

class FakeOffline final : public Plugin {
public:
    [[nodiscard]] std::vector<std::string> schemes() const override { return {"sftp"}; }
};

std::unique_ptr<Plugin> make_shares(Host& /*host*/) { return std::make_unique<FakeShares>(); }

std::unique_ptr<Plugin> make_offline(Host& /*host*/) { return std::make_unique<FakeOffline>(); }

const std::array<Builtin, 2> kFakes{{
    {.id = "offline", .summary = "sftp, never available", .make = &make_offline},
    {.id = "shares", .summary = "smb", .make = &make_shares},
}};

Place place(Place::Section section, std::string label, std::filesystem::path path,
            std::string uri = {}) {
    Place out;
    out.section = section;
    out.label = std::move(label);
    out.path = std::move(path);
    out.uri = std::move(uri);
    return out;
}

Place remote(std::string uri) { return place(Place::Section::bookmarks, uri, {}, uri); }

}  // namespace

TEST_CASE("plugins: every one built in runs unless disabled; unknown names are ignored") {
    EventLoop loop;
    const Plugins all{Host{.loop = loop, .places_changed = [] {}}, {}, kFakes};
    CHECK(all.running() == std::vector<std::string_view>{"offline", "shares"});

    const Plugins some{Host{.loop = loop, .places_changed = [] {}}, {"shares", "nope"}, kFakes};
    CHECK(some.running() == std::vector<std::string_view>{"offline"});
}

TEST_CASE("plugins: remote bookmarks show only for a scheme a running plugin mounts") {
    EventLoop loop;
    const std::vector<Place> places{place(Place::Section::places, "Home", "/home/me"),
                                    remote("smb://nas/public"), remote("sftp://me@host/srv"),
                                    remote("dav://web/files")};

    const Plugins all{Host{.loop = loop, .places_changed = [] {}}, {}, kFakes};
    const std::vector<Place> shown = all.places(places);
    REQUIRE(shown.size() == 3);
    CHECK(shown[0].label == "Home");
    CHECK(shown[1].uri == "smb://nas/public");
    CHECK(shown[1].path == "/run/fake/share");  // filled in by its plugin
    CHECK(shown[1].disk.mounted);
    CHECK(shown[2].uri == "sftp://me@host/srv");  // claimed, though it cannot mount now

    const Plugins none{Host{.loop = loop, .places_changed = [] {}}, {"shares", "offline"}, kFakes};
    const std::vector<Place> local = none.places(places);
    REQUIRE(local.size() == 1);
    CHECK(local[0].label == "Home");
}

TEST_CASE("plugins: a mount goes to the plugin of its scheme, when one can mount") {
    EventLoop loop;
    g_mounted.clear();
    Plugins all{Host{.loop = loop, .places_changed = [] {}}, {}, kFakes};
    std::optional<Remotes> remotes = all.remotes();
    REQUIRE(remotes);

    std::string where;
    std::string error;
    const auto done = [&](const std::filesystem::path& path, const std::string& failure) {
        where = path.string();
        error = failure;
    };
    remotes->mount("smb://nas/public", {}, done);
    CHECK(g_mounted == std::vector<std::string>{"smb://nas/public"});
    CHECK(where == "/run/fake/share");
    CHECK(error.empty());

    remotes->mount("sftp://me@host/srv", {}, done);  // its plugin cannot mount now
    CHECK(error == "not available");
    remotes->mount("dav://web/files", {}, done);  // no plugin for it
    CHECK(error == "not available");
    CHECK(g_mounted.size() == 1);

    Plugins offline{Host{.loop = loop, .places_changed = [] {}}, {"shares"}, kFakes};
    CHECK_FALSE(offline.remotes());
}

TEST_CASE("plugins: [plugins] disabled in config.toml") {
    const auto result =
        nordend::core::parse_config("[plugins]\ndisabled = [\"remotes\", 3]\n", "config.toml");
    CHECK(result.config.plugins.disabled == std::vector<std::string>{"remotes"});
    REQUIRE(result.problems.size() == 1);
    CHECK(result.problems[0].find("plugins.disabled") != std::string::npos);

    CHECK(nordend::core::parse_config("", "config.toml").config.plugins.disabled.empty());
}
