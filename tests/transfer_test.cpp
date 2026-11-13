#include "rummage/fs/transfer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/transfer_queue.hpp"
#include "temp_dir.hpp"

using rummage::core::CancelToken;
using rummage::core::EventLoop;
using rummage::fs::Conflict;
using rummage::fs::Resolution;
using rummage::fs::TransferCallbacks;
using rummage::fs::TransferKind;
using rummage::fs::TransferProgress;
using rummage::fs::TransferQueue;
using rummage::fs::TransferRequest;
using rummage::fs::TransferResult;
using rummage::test::TempDir;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

using Names = std::vector<std::string>;

bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(10s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

Names listing(const fs::path& directory) {
    Names names;
    for (const fs::directory_entry& entry : fs::directory_iterator{directory}) {
        names.push_back(entry.path().filename().string());
    }
    std::ranges::sort(names);
    return names;
}

std::string read_file(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

struct stat stat_of(const fs::path& path) {
    struct stat info{};
    REQUIRE(lstat(path.c_str(), &info) == 0);
    return info;
}

// Answers every conflict the same way, counting them.
struct Answer {
    Resolution resolution{Resolution::skip};
    int asked{0};

    TransferCallbacks callbacks() {
        return {.progress = nullptr, .resolve = [this](const Conflict& /*conflict*/) {
                    ++asked;
                    return resolution;
                }};
    }
};

// from/{P1.ORF, P1.ORF.xmp, P1_01.ORF.xmp, notes.txt, link -> notes.txt, Trip/{a.jpg, deep/b.jpg}}
// and an empty to/.
struct Tree {
    TempDir dir;
    fs::path from = dir.path / "from";
    fs::path to = dir.path / "to";

    Tree() {
        fs::create_directories(from / "Trip" / "deep");
        fs::create_directories(to);
        for (const char* name : {"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "notes.txt", "Trip/a.jpg",
                                 "Trip/deep/b.jpg"}) {
            std::ofstream{from / name} << name;
        }
        fs::create_symlink("notes.txt", from / "link");
        // An old date and odd permissions, to see them kept.
        const std::array<timespec, 2> times{timespec{.tv_sec = 1'000'000'000, .tv_nsec = 5},
                                            timespec{.tv_sec = 1'100'000'000, .tv_nsec = 7}};
        REQUIRE(utimensat(AT_FDCWD, (from / "P1.ORF").c_str(), times.data(), 0) == 0);
        fs::permissions(from / "notes.txt",
                        fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    }

    TransferResult run(TransferKind kind, Names names, const TransferCallbacks& callbacks = {},
                       bool force_copy = false, const CancelToken& token = {}) {
        return rummage::fs::run_transfer({.kind = kind,
                                          .source = from,
                                          .names = std::move(names),
                                          .destination = to,
                                          .force_copy = force_copy},
                                         token, callbacks);
    }
};

}  // namespace

TEST_CASE("transfer: a copy keeps contents, dates, permissions, links; sidecars come along") {
    Tree t;
    std::vector<TransferProgress> seen;
    const TransferResult result =
        t.run(TransferKind::copy, {"P1.ORF", "notes.txt", "link", "Trip"},
              {.progress = [&](const TransferProgress& progress) { seen.push_back(progress); },
               .resolve = nullptr});
    CHECK(result.refused.empty());
    CHECK(result.errors.empty());
    CHECK(result.placed == Names{"P1.ORF", "notes.txt", "link", "Trip"});
    CHECK(listing(t.to) ==
          Names{"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "Trip", "link", "notes.txt"});
    CHECK(listing(t.to / "Trip") == Names{"a.jpg", "deep"});
    CHECK(read_file(t.to / "Trip" / "deep" / "b.jpg") == "Trip/deep/b.jpg");
    CHECK(read_file(t.to / "P1_01.ORF.xmp") == "P1_01.ORF.xmp");
    CHECK(fs::read_symlink(t.to / "link") == "notes.txt");
    CHECK(stat_of(t.to / "P1.ORF").st_mtim.tv_sec == 1'100'000'000);
    CHECK(stat_of(t.to / "P1.ORF").st_mtim.tv_nsec == 7);
    CHECK((stat_of(t.to / "notes.txt").st_mode & 0777) == 0640);
    CHECK(listing(t.from).size() == 6);  // the sources stay

    // Progress: adding up first, then every file, all of the bytes.
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.front().preparing);
    CHECK(seen.back().files_done == 7);
    CHECK(seen.back().files_total == 7);
    CHECK(seen.back().bytes_done == seen.back().bytes_total);
    CHECK(seen.back().bytes_total > 0);
}

TEST_CASE("transfer: a move on one disk renames; between disks it copies, then deletes") {
    Tree t;
    const ino_t inode = stat_of(t.from / "P1.ORF").st_ino;
    TransferResult result = t.run(TransferKind::move, {"P1.ORF", "Trip"});
    CHECK(result.errors.empty());
    CHECK(stat_of(t.to / "P1.ORF").st_ino == inode);  // the same file: renamed
    CHECK(listing(t.to) == Names{"P1.ORF", "P1.ORF.xmp", "P1_01.ORF.xmp", "Trip"});
    CHECK(listing(t.from) == Names{"link", "notes.txt"});

    // The path between disks, forced: a new file, the old one gone, the folder too.
    Tree u;
    const ino_t before = stat_of(u.from / "P1.ORF").st_ino;
    result = u.run(TransferKind::move, {"P1.ORF", "Trip", "link"}, {}, true);
    CHECK(result.errors.empty());
    CHECK(stat_of(u.to / "P1.ORF").st_ino != before);
    CHECK(stat_of(u.to / "P1.ORF").st_mtim.tv_sec == 1'100'000'000);
    CHECK(read_file(u.to / "Trip" / "deep" / "b.jpg") == "Trip/deep/b.jpg");
    CHECK(fs::is_symlink(u.to / "link"));
    CHECK(listing(u.from) == Names{"notes.txt"});

    // Into the folder it is in: nothing to do.
    result = rummage::fs::run_transfer({.kind = TransferKind::move,
                                        .source = u.from,
                                        .names = {"notes.txt"},
                                        .destination = u.from},
                                       {}, {});
    CHECK(result.placed == Names{"notes.txt"});
}

TEST_CASE("transfer: conflicts are asked; replace, skip, keep both, cancel") {
    Tree t;
    std::ofstream{t.to / "notes.txt"} << "old notes";
    std::ofstream{t.to / "P1.ORF.xmp"} << "someone else's sidecar";
    Answer skip{.resolution = Resolution::skip};
    TransferResult result = t.run(TransferKind::copy, {"notes.txt", "P1.ORF"}, skip.callbacks());
    CHECK(skip.asked == 2);  // P1.ORF too: its sidecar's name is taken
    CHECK(result.placed.empty());
    CHECK(read_file(t.to / "notes.txt") == "old notes");

    Answer both{.resolution = Resolution::keep_both};
    result = t.run(TransferKind::copy, {"notes.txt", "P1.ORF"}, both.callbacks());
    CHECK(result.placed == Names{"notes 2.txt", "P1 2.ORF"});
    CHECK(read_file(t.to / "P1 2.ORF.xmp") == "P1.ORF.xmp");  // the sidecars follow the new name
    CHECK(read_file(t.to / "P1 2_01.ORF.xmp") == "P1_01.ORF.xmp");

    Answer replace{.resolution = Resolution::replace};
    result = t.run(TransferKind::copy, {"notes.txt", "P1.ORF"}, replace.callbacks());
    CHECK(result.errors.empty());
    CHECK(read_file(t.to / "notes.txt") == "notes.txt");
    CHECK(read_file(t.to / "P1.ORF.xmp") == "P1.ORF.xmp");

    Answer cancel{.resolution = Resolution::cancel};
    result = t.run(TransferKind::copy, {"notes.txt", "Trip"}, cancel.callbacks());
    CHECK(result.cancelled);
    CHECK_FALSE(fs::exists(t.to / "Trip"));  // the next item never started
}

TEST_CASE("transfer: folders merge on replace; a file never replaces a folder") {
    Tree t;
    fs::create_directories(t.to / "Trip");
    std::ofstream{t.to / "Trip" / "a.jpg"} << "old";
    std::ofstream{t.to / "Trip" / "mine.jpg"} << "mine";
    Answer replace{.resolution = Resolution::replace};
    TransferResult result = t.run(TransferKind::copy, {"Trip"}, replace.callbacks());
    CHECK(result.errors.empty());
    CHECK(listing(t.to / "Trip") == Names{"a.jpg", "deep", "mine.jpg"});
    CHECK(read_file(t.to / "Trip" / "a.jpg") == "Trip/a.jpg");

    fs::create_directories(t.to / "x");
    fs::rename(t.to / "x", t.to / "link");  // a folder where the link would go
    result = t.run(TransferKind::copy, {"link"}, replace.callbacks());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors.front() == "link: a folder has its name; a file does not replace a folder");
    CHECK(fs::is_directory(t.to / "link"));
}

TEST_CASE("transfer: a copy into its own folder keeps both by itself") {
    Tree t;
    Answer never{.resolution = Resolution::cancel};
    const TransferResult result = rummage::fs::run_transfer({.kind = TransferKind::copy,
                                                             .source = t.from,
                                                             .names = {"P1.ORF", "Trip"},
                                                             .destination = t.from},
                                                            {}, never.callbacks());
    CHECK(never.asked == 0);
    CHECK(result.placed == Names{"P1 2.ORF", "Trip 2"});
    CHECK(fs::exists(t.from / "P1 2.ORF.xmp"));
    CHECK(fs::exists(t.from / "Trip 2" / "deep" / "b.jpg"));
}

TEST_CASE("transfer: refused before anything moves") {
    Tree t;
    TransferResult result = rummage::fs::run_transfer({.kind = TransferKind::copy,
                                                       .source = t.from,
                                                       .names = {"Trip"},
                                                       .destination = t.from / "Trip" / "deep"},
                                                      {}, {});
    CHECK(result.refused == "Cannot copy Trip into itself");
    CHECK(listing(t.from / "Trip" / "deep") == Names{"b.jpg"});

    // Exceed free space, rather than assuming the filesystem permits a 1 PiB file.
    {
        std::ofstream{t.from / "huge.raw"};
    }
    fs::resize_file(t.from / "huge.raw", fs::space(t.to).available + (std::uintmax_t{1} << 30U));
    result = t.run(TransferKind::copy, {"huge.raw"});
    CHECK(result.refused.starts_with("Not enough space: "));
    CHECK(listing(t.to).empty());
}

TEST_CASE("transfer: cancelled halfway, nothing half-written is left") {
    Tree t;
    {
        std::ofstream big{t.from / "big.bin"};
        const std::string block(1 << 20, 'x');
        for (int i = 0; i < 40; ++i) {
            big << block;  // 40 MB: five pieces of 8 MB
        }
    }
    const CancelToken token;
    const TransferResult result = t.run(TransferKind::move, {"big.bin", "Trip"},
                                        {.progress =
                                             [&](const TransferProgress& progress) {
                                                 if (progress.bytes_done > 0) {
                                                     token.cancel();  // after the first piece
                                                 }
                                             },
                                         .resolve = nullptr},
                                        true, token);
    CHECK(result.cancelled);
    CHECK(listing(t.to).empty());                            // no temporary, no partial file
    CHECK(fs::file_size(t.from / "big.bin") == 40U << 20U);  // the source is whole
}

TEST_CASE("transfer: a folder without write permission gives errors, not a crash") {
    if (geteuid() == 0) {
        return;
    }
    Tree t;
    fs::permissions(t.to, fs::perms::owner_read | fs::perms::owner_exec);
    const TransferResult result = t.run(TransferKind::copy, {"notes.txt", "Trip"});
    fs::permissions(t.to, fs::perms::owner_all);
    REQUIRE(result.errors.size() == 2);
    CHECK(result.errors.front() == "notes.txt: Permission denied");
    CHECK(result.placed.empty());
}

TEST_CASE("transfer queue: progress, a question answered later, the result") {
    Tree t;
    std::ofstream{t.to / "notes.txt"} << "old";
    EventLoop loop;
    int progress_seen = 0;
    std::optional<std::function<void(Resolution)>> answer;
    std::optional<TransferResult> done;
    TransferQueue queue{loop,
                        [&](const TransferRequest& /*request*/,
                            const TransferProgress& /*progress*/) { ++progress_seen; },
                        [&](const Conflict& conflict, std::function<void(Resolution)> reply) {
                            CHECK(conflict.name == "notes.txt");
                            answer = std::move(reply);
                        },
                        [&](const TransferRequest& request, const TransferResult& result) {
                            CHECK(request.names == Names{"notes.txt", "Trip"});
                            done = result;
                        }};
    queue.submit({.kind = TransferKind::copy,
                  .source = t.from,
                  .names = {"notes.txt", "Trip"},
                  .destination = t.to});
    CHECK(queue.pending() == 1);
    REQUIRE(run_until(loop, [&] { return answer.has_value(); }));
    CHECK_FALSE(done.has_value());  // waiting for the answer
    answer.value_or(std::function<void(Resolution)>{})(Resolution::keep_both);
    REQUIRE(run_until(loop, [&] { return done.has_value(); }));
    CHECK(queue.pending() == 0);
    CHECK(done.value_or(TransferResult{}).placed == Names{"notes 2.txt", "Trip"});
    CHECK(progress_seen > 0);

    // Gone while a question waits: the transfer is cancelled and the thread joins.
    answer.reset();
    auto second = std::make_unique<TransferQueue>(
        loop, nullptr,
        [&](const Conflict& /*conflict*/, std::function<void(Resolution)> reply) {
            answer = std::move(reply);
        },
        nullptr);
    second->submit({.kind = TransferKind::copy,
                    .source = t.from,
                    .names = {"notes.txt"},
                    .destination = t.to});
    REQUIRE(run_until(loop, [&] { return answer.has_value(); }));
    second.reset();
    answer.value_or(std::function<void(Resolution)>{})(Resolution::replace);  // too late
    CHECK(read_file(t.to / "notes.txt") == "old");
}
