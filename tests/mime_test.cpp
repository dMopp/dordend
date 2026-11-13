#include "rummage/fs/mime.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/sort.hpp"
#include "temp_dir.hpp"

using rummage::fs::Entry;
using rummage::fs::kUnknownMime;
using rummage::fs::MimeDatabase;
using rummage::fs::MimeId;
using rummage::test::TempDir;

namespace {

// One magic rule line: indent, offset, value, optional mask / word size / range.
std::string rule(int indent, int offset, std::string_view value, std::string_view mask = {},
                 int word = 1, int range = 1) {
    std::string out = indent > 0 ? std::to_string(indent) : "";
    out += ">" + std::to_string(offset) + "=";
    out += static_cast<char>((value.size() >> 8U) & 0xFFU);
    out += static_cast<char>(value.size() & 0xFFU);
    out += value;
    if (!mask.empty()) {
        out += "&";
        out += mask;
    }
    if (word != 1) {
        out += "~" + std::to_string(word);
    }
    if (range != 1) {
        out += "+" + std::to_string(range);
    }
    return out + "\n";
}

// A small database in two mime directories: "high" overrides "low".
struct Fixture {
    TempDir dir;
    std::shared_ptr<const MimeDatabase> db;

    Fixture() {
        std::filesystem::create_directories(dir.path / "high");
        std::filesystem::create_directories(dir.path / "low");
        dir.write("low/globs2",
                  "# comment\n"
                  "50:image/x-olympus-orf:*.orf\n"
                  "50:image/jpeg:*.jpg\n"
                  "50:application/gzip:*.gz\n"
                  "50:application/x-compressed-tar:*.tar.gz\n"
                  "60:text/x-python:*.py\n"
                  "50:text/x-python3:*.py\n"
                  "50:text/x-csrc:*.c:cs\n"
                  "50:text/x-c++src:*.C:cs\n"
                  "50:text/x-c++src:*.C\n"
                  "50:text/x-csrc:*.c\n"
                  "50:text/x-makefile:makefile\n"
                  "10:text/x-readme:readme*\n"
                  "50:image/x-panasonic-rw:*.raw\n"
                  "50:application/x-old:*.old\n"
                  "50:text/x-a:*.same\n"
                  "50:text/x-b:*.same\n");
        dir.write("high/globs2",
                  "50:application/x-old:__NOGLOBS__\n"
                  "50:application/x-new:*.old\n");
        dir.write("low/aliases", "image/jpg image/jpeg\n");
        dir.write("low/subclasses",
                  "application/x-compressed-tar application/gzip\n"
                  "text/x-python3 text/x-python\n");
        std::string magic{"MIME-Magic\0\n", 12};
        magic += "[60:image/png]\n" + rule(0, 0, "\x89PNG");
        magic += "[50:application/gzip]\n" + rule(0, 0, "\x1F\x8B");
        magic += "[50:text/x-python3]\n" + rule(0, 0, "#!/usr/bin/python3");
        // Children: "ABC" at 0 and then "!" at 3 or "?" at 3.
        magic +=
            "[40:application/x-tree]\n" + rule(0, 0, "ABC") + rule(1, 3, "!") + rule(1, 3, "?");
        // Masked, little-endian 16-bit word, searched over a range.
        magic += "[30:application/x-masked]\n" + rule(0, 0, "\x0F\xF0", "\x0F\xFF");
        magic += "[30:application/x-word]\n" + rule(0, 0, std::string{"\x12\x34", 2}, {}, 2);
        magic += "[30:application/x-range]\n" + rule(0, 0, "NEEDLE", {}, 1, 10);
        magic += "[50:application/x-desktop]\n" + rule(0, 0, "[Desktop Entry]");
        dir.write("low/magic", magic);
        db = MimeDatabase::load({dir.path / "high", dir.path / "low"});
    }

    [[nodiscard]] std::string_view by_name(std::string_view name) const {
        return db->name(db->by_name(name).mime);
    }

    [[nodiscard]] bool certain(std::string_view name) const { return db->by_name(name).certain; }

    [[nodiscard]] std::string_view sniff(std::string_view head) const {
        return db->name(db->by_content(std::as_bytes(std::span{head})));
    }

    [[nodiscard]] std::string_view detect(std::string_view name, std::string_view head) const {
        return db->name(db->detect(name, std::as_bytes(std::span{head}), head.size()));
    }
};

}  // namespace

TEST_CASE("mime: names") {
    const Fixture f;
    CHECK(f.by_name("P1010001.ORF") == "image/x-olympus-orf");  // case-insensitive glob
    CHECK(f.certain("P1010001.ORF"));
    CHECK(f.by_name("photo.jpg") == "image/jpeg");
    CHECK(f.by_name("backup.tar.gz") == "application/x-compressed-tar");  // longest suffix
    CHECK(f.certain("backup.tar.gz"));
    CHECK(f.by_name("Makefile") == "text/x-makefile");  // literal
    CHECK(f.by_name("unknown.xyz").empty());
    CHECK_FALSE(f.certain("unknown.xyz"));
}

TEST_CASE("mime: case-sensitive globs win, full patterns only without a suffix match") {
    const Fixture f;
    CHECK(f.by_name("main.c") == "text/x-csrc");
    CHECK(f.certain("main.c"));
    CHECK(f.by_name("main.C") == "text/x-c++src");
    CHECK(f.certain("main.C"));
    CHECK(f.by_name("README.raw") == "image/x-panasonic-rw");  // not readme*
    CHECK(f.by_name("README") == "text/x-readme");
}

TEST_CASE("mime: several types for a name are not certain; order is weight, then file") {
    const Fixture f;
    CHECK(f.by_name("script.py") == "text/x-python");  // weight 60 first
    CHECK_FALSE(f.certain("script.py"));
    CHECK(f.by_name("x.same") == "text/x-a");  // equal weight: file order
    CHECK_FALSE(f.certain("x.same"));
}

TEST_CASE("mime: __NOGLOBS__ in a higher directory drops the lower globs") {
    const Fixture f;
    CHECK(f.by_name("file.old") == "application/x-new");
    CHECK(f.certain("file.old"));
}

TEST_CASE("mime: aliases and subclasses") {
    const Fixture f;
    CHECK(f.db->find("image/jpg") == f.db->find("image/jpeg"));
    const MimeId tar = f.db->find("application/x-compressed-tar");
    const MimeId gzip = f.db->find("application/gzip");
    CHECK(f.db->is_a(tar, gzip));
    CHECK_FALSE(f.db->is_a(gzip, tar));
    CHECK(f.db->is_a(f.db->find("text/x-makefile"), MimeDatabase::kTextPlain));
    CHECK(f.db->is_a(tar, MimeDatabase::kOctetStream));
    CHECK_FALSE(f.db->is_a(MimeDatabase::kDirectory, MimeDatabase::kOctetStream));
    CHECK(f.db->find("no/such-type") == kUnknownMime);
}

TEST_CASE("mime: magic rules") {
    const Fixture f;
    CHECK(f.sniff("\x89PNG\r\n") == "image/png");
    CHECK(f.sniff("ABC!") == "application/x-tree");
    CHECK(f.sniff("ABC?") == "application/x-tree");
    CHECK(f.sniff("ABC.").empty());                        // the parent matches, no child does
    CHECK(f.sniff("\x0A\xF0").empty());                    // 0x0A & 0x0F != 0x0F
    CHECK(f.sniff("\xFF\xF0") == "application/x-masked");  // bits outside the mask ignored
    CHECK(f.sniff("\x0F\xF0") == "application/x-masked");
    CHECK(f.sniff(std::string{"\x34\x12", 2}) == "application/x-word");  // big-endian rule
    CHECK(f.sniff("xxxxNEEDLE") == "application/x-range");               // offset 4 in 0..9
    CHECK(f.sniff("xxxxxxxxxxNEEDLE").empty());                          // offset 10: out
    CHECK(f.db->magic_extent() == 18);  // "#!/usr/bin/python3", the longest reach
}

TEST_CASE("mime: detection order") {
    const Fixture f;
    CHECK(f.detect("photo.jpg", "anything") == "image/jpeg");  // a certain name: no sniffing
    CHECK(f.detect("script.py", "#!/usr/bin/python3\n") == "text/x-python3");
    CHECK(f.detect("script.py", "print(1)\n") == "text/x-python");  // text: first name type
    CHECK(f.detect("noext", "\x89PNG\r\n") == "image/png");
    CHECK(f.detect("noext", "plain words\n") == "text/plain");
    CHECK(f.detect("noext", std::string{"\x01\x02\x03", 3}) == "application/octet-stream");
    CHECK(f.detect("empty.jpg", "") == "application/x-zerosize");
    CHECK(f.detect("launcher", "[Desktop Entry]\nExec=x\n") == "text/plain");  // not *.desktop
    CHECK(f.detect("app.desktop", "[Desktop Entry]\n") == "application/x-desktop");
}

TEST_CASE("mime: detect() on entries, and the loader's name-only types") {
    const Fixture f;
    const TempDir files;
    files.write("image.png", "\x89PNG\r\n");
    files.write("noext-png", "\x89PNG\r\n");
    files.write("empty.jpg", "");
    std::filesystem::create_directory(files.path / "sub");

    std::vector<Entry> entries;
    rummage::fs::read_directory(files.path, rummage::core::CancelToken{}, {.mime = f.db},
                                [&](std::vector<Entry> chunk) {
                                    for (Entry& e : chunk) {
                                        entries.push_back(std::move(e));
                                    }
                                });
    REQUIRE(entries.size() == 4);
    for (const Entry& entry : entries) {
        const std::string_view loaded = f.db->name(entry.mime);
        const std::string_view detected = f.db->name(f.db->detect(files.path, entry));
        if (entry.name == "sub") {
            CHECK(loaded == "inode/directory");
        } else if (entry.name == "empty.jpg") {
            CHECK(loaded == "application/x-zerosize");
        } else if (entry.name == "noext-png") {
            CHECK(entry.mime == kUnknownMime);  // needs the content
            CHECK(detected == "image/png");
        } else {
            CHECK(entry.mime == kUnknownMime);  // *.png has no glob in this database
            CHECK(detected == "image/png");
        }
    }
}

TEST_CASE("mime: a name with several types sorts by type with its best one, sniffed or not") {
    const Fixture f;
    const TempDir files;
    files.write("script.py", "print(1)\n");  // text/x-python (60) or text/x-python3 (50)
    files.write("main.c", "int x;\n");
    files.write("photo.jpg", "jpeg");
    files.write("noext", "data");

    std::vector<Entry> entries;
    rummage::fs::read_directory(files.path, rummage::core::CancelToken{}, {.mime = f.db},
                                [&](std::vector<Entry> chunk) {
                                    for (Entry& e : chunk) {
                                        entries.push_back(std::move(e));
                                    }
                                });
    REQUIRE(entries.size() == 4);
    for (const Entry& entry : entries) {
        INFO(entry.name);
        if (entry.name == "script.py") {
            CHECK(entry.mime == kUnknownMime);  // still sniffed for its icon and to open it
            CHECK(f.db->name(entry.name_guess) == "text/x-python");
        } else {
            CHECK(entry.name_guess == kUnknownMime);
        }
    }
    std::ranges::sort(
        entries, rummage::fs::SortOrder{.key = rummage::fs::SortKey::type, .mime = f.db.get()});
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const Entry& entry : entries) {
        names.push_back(entry.name);
    }
    // Not after the unknown ones: among the text types, by its best guess.
    CHECK(names == std::vector<std::string>{"photo.jpg", "main.c", "script.py", "noext"});
}

TEST_CASE("mime: the system database loads") {
    const auto db = MimeDatabase::load();
    if (db->glob_count() == 0) {
        MESSAGE("no shared-mime-info on this system: skipped");
        return;
    }
    CHECK(db->name(db->by_name("IMG_0001.ORF").mime) == "image/x-olympus-orf");
    CHECK(db->name(db->by_name("photo.JPG").mime) == "image/jpeg");
    CHECK(db->magic_count() > 100);
}
