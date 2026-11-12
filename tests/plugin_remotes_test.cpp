// The remotes plugin's gvfs side (plugins/remotes): URIs to gvfs mounts, matching, local folders,
// and the real session bus. Built only with NORDEND_PLUGIN_REMOTES.

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "nordend/core/event_loop.hpp"
#include "nordend/fs/places.hpp"
#include "nordend/plugins/remotes/gvfs.hpp"

using nordend::core::EventLoop;
using nordend::fs::parse_remote_uri;
using nordend::fs::Place;
using nordend::fs::RemoteUri;
using nordend::plugins::remotes::gvfs_location;
using nordend::plugins::remotes::GvfsLocation;
using nordend::plugins::remotes::GvfsMount;
using nordend::plugins::remotes::GvfsMountSpec;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace remotes = nordend::plugins::remotes;

namespace {

// gvfs_location of a URI that parses.
GvfsLocation location_of(std::string_view uri) {
    const std::optional<RemoteUri> remote = parse_remote_uri(uri);
    REQUIRE(remote);
    const std::optional<GvfsLocation> location = gvfs_location(remote.value_or(RemoteUri{}));
    REQUIRE(location);
    return location.value_or(GvfsLocation{});
}

GvfsMount share_mount(std::string server, std::string share, fs::path fuse) {
    return {.dbus_id = ":1.42",
            .object_path = "/org/gtk/vfs/mount/1",
            .name = share + " on " + server,
            .fuse = std::move(fuse),
            .spec = {.type = "smb-share",
                     .items = {{"server", std::move(server)}, {"share", std::move(share)}},
                     .prefix = "/"}};
}

}  // namespace

TEST_CASE("gvfs: the mount of an smb URI, as gvfs's own mapper makes it") {
    // A share: server and share case-folded, the rest the path inside.
    CHECK(location_of("smb://FileShare.Example/Public/Scans/2026/") ==
          GvfsLocation{.spec = {.type = "smb-share",
                                .items = {{"server", "fileshare.example"}, {"share", "public"}},
                                .prefix = "/"},
                       .path = "/Scans/2026/"});
    CHECK(location_of("smb://nas/Public").path == "/");
    // "DOMAIN;user" split; the default port left out, another kept.
    CHECK(location_of("smb://WORK;me@nas:445/share").spec.items ==
          std::map<std::string, std::string>{
              {"server", "nas"}, {"share", "share"}, {"domain", "WORK"}, {"user", "me"}});
    CHECK(location_of("smb://;me@nas:4450/share").spec.items ==
          std::map<std::string, std::string>{
              {"server", "nas"}, {"share", "share"}, {"user", "me"}, {"port", "4450"}});
    // The server alone lists its shares.
    CHECK(location_of("smb://nas/").spec ==
          GvfsMountSpec{.type = "smb-server", .items = {{"server", "nas"}}, .prefix = "/"});
}

TEST_CASE("gvfs: the mount of an sftp URI") {
    CHECK(location_of("sftp://me@host:2222/srv/www") ==
          GvfsLocation{.spec = {.type = "sftp",
                                .items = {{"host", "host"}, {"user", "me"}, {"port", "2222"}},
                                .prefix = "/"},
                       .path = "/srv/www"});
    CHECK(location_of("sftp://host").spec.items ==
          std::map<std::string, std::string>{{"host", "host"}});
}

TEST_CASE("gvfs: a mount matches when it has every key wanted, and gives the local folder") {
    const GvfsLocation wanted = location_of("smb://fileshare.example/Public/Scans");
    GvfsMount mount = share_mount("fileshare.example", "public",
                                  "/run/user/1000/gvfs/smb-share:server=fileshare.example,"
                                  "share=public");
    mount.spec.items.emplace("user", "me");  // who logged in: more keys still match
    CHECK(remotes::spec_matches(wanted.spec, mount.spec));
    CHECK_FALSE(remotes::spec_matches(mount.spec, wanted.spec));
    CHECK_FALSE(
        remotes::spec_matches(location_of("smb://fileshare.example/Dokumente").spec, mount.spec));
    CHECK_FALSE(remotes::spec_matches(location_of("sftp://fileshare.example").spec, mount.spec));

    const std::vector<GvfsMount> mounts{share_mount("other", "public", "/x"), mount};
    const GvfsMount* const holding = remotes::mount_holding(mounts, wanted);
    REQUIRE(holding != nullptr);
    CHECK(remotes::local_path(*holding, wanted) ==
          "/run/user/1000/gvfs/smb-share:server=fileshare.example,share=public/Scans");
    CHECK(remotes::local_path(*holding, location_of("smb://fileshare.example/public/")) ==
          "/run/user/1000/gvfs/smb-share:server=fileshare.example,share=public");
    // Without gvfsd-fuse there is no local folder.
    mount.fuse.clear();
    CHECK(remotes::local_path(mount, wanted).empty());
    CHECK(remotes::mount_holding({}, wanted) == nullptr);
}

TEST_CASE("gvfs: remote bookmarks get their folder while mounted, and an eject icon") {
    std::vector<Place> places = nordend::fs::parse_bookmarks(
        "file:///tmp/a\n"
        "smb://fileshare.example/Public\n"
        "smb://fileshare.example/Dokumente/Steuer Steuer\n");
    const std::vector<GvfsMount> mounts{share_mount("fileshare.example", "dokumente", "/fuse/d")};
    places = remotes::with_remote_mounts(std::move(places), mounts);
    REQUIRE(places.size() == 3);
    CHECK(places[0].path == "/tmp/a");
    CHECK(places[0].disk.mounted);  // a local one stays as it is
    CHECK(places[1].path.empty());
    CHECK_FALSE(places[1].disk.mounted);
    CHECK_FALSE(places[1].disk.ejectable);
    CHECK(places[2].path == "/fuse/d/Steuer");
    CHECK(places[2].disk.mounted);
    CHECK(places[2].disk.ejectable);
    // Unmounted elsewhere: back without a path.
    places = remotes::with_remote_mounts(std::move(places), {});
    CHECK(places[2].path.empty());
    CHECK_FALSE(places[2].disk.mounted);
}

TEST_CASE("gvfs: the real session bus answers without starting gvfsd (reads only)") {
    EventLoop loop;
    remotes::Gvfs gvfs{loop, [] {}};
    if (!gvfs.available()) {
        MESSAGE("no session bus: skipped");
        return;
    }
    // The first answers come back through the loop; most of the time nothing is mounted.
    const auto stop = loop.add_timer(300ms, 0ms, [&loop] { loop.quit(0); });
    loop.run();
    loop.remove(stop);
    for (const GvfsMount& mount : gvfs.mounts()) {
        CHECK_FALSE(mount.spec.type.empty());
        MESSAGE(mount.name << " (" << mount.spec.type << ") at " << mount.fuse.string());
    }
}
