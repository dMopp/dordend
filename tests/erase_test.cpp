#include "rummage/fs/erase.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/transfer.hpp"
#include "rummage/fs/trash.hpp"
#include "temp_dir.hpp"

using rummage::core::CancelToken;
using rummage::fs::ErasePlan;
using rummage::fs::TransferKind;
using rummage::fs::TransferRequest;
using rummage::fs::TransferResult;
using rummage::test::TempDir;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

Names listing(const fs::path& directory) {
    Names names;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

void make(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream{file} << text;
}

TransferRequest erase(const fs::path& source, Names names) {
    return {.kind = TransferKind::erase,
            .source = source,
            .names = std::move(names),
            .destination = {},
            .force_copy = false};
}

void trash(const fs::path& source, const Names& names, const fs::path& home,
           const fs::path& top = {}) {
    const TransferResult result = rummage::fs::run_trash({.kind = TransferKind::trash,
                                                          .source = source,
                                                          .names = names,
                                                          .destination = home,
                                                          .force_copy = false,
                                                          .trash_top = top},
                                                         CancelToken{}, {});
    REQUIRE(result.errors.empty());
    REQUIRE(result.refused.empty());
}

}  // namespace

TEST_CASE("erase: files and folders go for good, sidecars along, links not followed") {
    const TempDir dir;
    const fs::path photos = dir.path / "photos";
    const fs::path elsewhere = dir.path / "elsewhere";
    make(photos / "P1.ORF", "raw");      // 3 bytes
    make(photos / "P1.ORF.xmp", "xmp");  // 3
    make(photos / "P2.ORF", "stays");
    make(photos / "Trip" / "a.txt", "aaaa");           // 4
    make(photos / "Trip" / "locked" / "b.txt", "bb");  // 2, in a folder that cannot be written
    fs::create_hard_link(photos / "Trip" / "a.txt", photos / "Trip" / "same.txt");
    make(elsewhere / "kept.txt", "kept");
    fs::create_directory_symlink(elsewhere, photos / "Trip" / "link");
    fs::permissions(photos / "Trip" / "locked", fs::perms::owner_read | fs::perms::owner_exec);

    const TransferRequest request = erase(photos, {"P1.ORF", "Trip", "missing"});
    const ErasePlan plan = rummage::fs::plan_erase(request, CancelToken{});
    CHECK(plan.names == Names{"P1.ORF", "Trip"});
    CHECK(plan.items ==
          std::vector<fs::path>{photos / "P1.ORF", photos / "P1.ORF.xmp", photos / "Trip"});
    CHECK(plan.sidecars == 1);
    // P1.ORF, its sidecar, Trip, a.txt, same.txt, locked, b.txt, link.
    CHECK(plan.entries == 8);
    CHECK(plan.bytes == 3 + 3 + 4 + 2);  // the hard link once
    CHECK(plan.errors.size() == 1);

    const TransferResult result = rummage::fs::run_erase(request, CancelToken{}, {});
    CHECK(result.errors.size() == 1);  // "missing"
    CHECK(result.placed == Names{"P1.ORF", "Trip"});
    CHECK(result.files_done == 8);
    CHECK(listing(photos) == Names{"P2.ORF"});
    CHECK(listing(elsewhere) == Names{"kept.txt"});  // the link went, not what it points to
}

TEST_CASE("erase: a sidecar picked with its photo counts once; cancelled, nothing goes") {
    const TempDir dir;
    make(dir.path / "P1.ORF", "raw");
    make(dir.path / "P1.ORF.xmp", "xmp");
    const TransferRequest request = erase(dir.path, {"P1.ORF.xmp", "P1.ORF"});
    const ErasePlan plan = rummage::fs::plan_erase(request, CancelToken{});
    CHECK(plan.names == Names{"P1.ORF"});
    CHECK(plan.items.size() == 2);
    CHECK(plan.sidecars == 0);  // asked for: nothing goes along unasked

    CancelToken stopped;
    stopped.cancel();
    const TransferResult result = rummage::fs::run_erase(request, stopped, {});
    CHECK(result.cancelled);
    CHECK(result.placed.empty());
    CHECK(listing(dir.path) == Names{"P1.ORF", "P1.ORF.xmp"});
}

TEST_CASE("erase: from the trash folder, the .trashinfo and directorysizes follow") {
    const TempDir dir;
    const fs::path home = dir.path / "Trash";
    const fs::path photos = dir.path / "photos";
    make(photos / "P1.ORF", "raw");
    make(photos / "P1.ORF.xmp", "xmp");
    make(photos / "Trip" / "a.txt", "a");
    make(photos / "Old" / "b.txt", "b");
    trash(photos, {"P1.ORF", "Trip", "Old"}, home);
    REQUIRE(listing(home / "files") == Names{"Old", "P1.ORF", "P1.ORF.xmp", "Trip"});

    const TransferRequest request{
        .kind = TransferKind::erase,
        .source = rummage::fs::trash_path(),
        .names = {(home / "files" / "P1.ORF").string(), (home / "files" / "Trip").string()},
        .destination = {},
        .force_copy = false};
    const ErasePlan plan = rummage::fs::plan_erase(request, CancelToken{});
    CHECK(plan.names == Names{"P1.ORF", "Trip"});
    CHECK(plan.sidecars == 1);
    const TransferResult result = rummage::fs::run_erase(request, CancelToken{}, {});
    CHECK(result.errors.empty());
    CHECK(result.placed == Names{"P1.ORF", "Trip"});
    CHECK(listing(home / "files") == Names{"Old"});
    CHECK(listing(home / "info") == Names{"Old.trashinfo"});
    std::ifstream sizes{home / "directorysizes"};
    std::string line;
    REQUIRE(std::getline(sizes, line));
    CHECK(line.ends_with(" Old"));
    CHECK_FALSE(std::getline(sizes, line));
}

TEST_CASE("erase: emptying the trash empties every trash, orphans and directorysizes too") {
    const TempDir dir;
    const fs::path home = dir.path / "Trash";
    const fs::path photos = dir.path / "photos";
    const fs::path disk = dir.path / "disk";
    make(photos / "a.txt", "a");
    make(photos / "Trip" / "b.txt", "b");
    make(disk / "shots" / "P1.ORF", "raw");
    trash(photos, {"a.txt", "Trip"}, home);
    trash(disk / "shots", {"P1.ORF"}, home, disk);
    const fs::path disk_trash = disk / std::format(".Trash-{}", geteuid());
    make(home / "info" / "gone.txt.trashinfo", "[Trash Info]\nPath=/gone.txt\n");  // an orphan
    make(disk_trash / "files" / "no-info.txt", "x");  // an item without its info: it goes too
    REQUIRE(fs::exists(home / "directorysizes"));

    TransferRequest request{.kind = TransferKind::empty_trash,
                            .source = rummage::fs::trash_path(),
                            .names = {home.string(), disk_trash.string()},
                            .destination = {},
                            .force_copy = false};
    const ErasePlan plan = rummage::fs::plan_erase(request, CancelToken{});
    CHECK(plan.items.size() == 4);
    CHECK(plan.entries == 5);  // a.txt, Trip, b.txt, P1.ORF, no-info.txt
    const TransferResult result = rummage::fs::run_erase(request, CancelToken{}, {});
    CHECK(result.errors.empty());
    CHECK(result.files_done == 5);
    for (const fs::path& trash : {home, disk_trash}) {
        CHECK(listing(trash / "files").empty());
        CHECK(listing(trash / "info").empty());
        CHECK_FALSE(fs::exists(trash / "directorysizes"));
    }
    CHECK(rummage::fs::list_trash(
              {.home = home, .tops = std::vector<fs::path>{disk}, .uid = geteuid()})
              .empty());
}
