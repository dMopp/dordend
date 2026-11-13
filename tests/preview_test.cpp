#include "rummage/preview/preview.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/mime.hpp"
#include "temp_dir.hpp"

namespace preview = rummage::preview;
namespace fs = std::filesystem;
using rummage::fs::MimeDatabase;
using rummage::test::TempDir;

namespace {

fs::path write(const fs::path& path, const std::string& bytes) {
    std::ofstream{path, std::ios::binary} << bytes;
    return path;
}

}  // namespace

TEST_CASE("preview: information on files, directories and links") {
    const TempDir dir;
    const rummage::core::CancelToken token;
    const fs::path file = write(dir.path / "a.txt", "a");
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read);
    const preview::Info info = preview::read_info(file, token);
    CHECK(info.error.empty());
    CHECK(info.permissions == "-rw-r-----");
    CHECK_FALSE(info.owner.empty());
    CHECK_FALSE(info.group.empty());
    CHECK_FALSE(info.items);

    fs::create_directory(dir.path / "sub");
    write(dir.path / "sub" / "x", "");
    write(dir.path / "sub" / ".hidden", "");
    fs::create_directory(dir.path / "sub" / "deeper");
    fs::permissions(dir.path / "sub", fs::perms::owner_all | fs::perms::sticky_bit);
    const preview::Info folder = preview::read_info(dir.path / "sub", token);
    CHECK(folder.permissions == "drwx-----T");
    CHECK(folder.items == std::optional<std::uint64_t>{3});
    CHECK(folder.hidden_items == 1);

    fs::create_symlink("sub", dir.path / "link");
    const preview::Info link = preview::read_info(dir.path / "link", token);
    CHECK(link.link_target == "sub");
    CHECK(link.permissions.front() == 'd');  // the target's
    fs::create_symlink("nowhere", dir.path / "dangling");
    const preview::Info dangling = preview::read_info(dir.path / "dangling", token);
    CHECK(dangling.link_target == "nowhere");
    CHECK(dangling.permissions.front() == 'l');

    CHECK_FALSE(preview::read_info(dir.path / "missing", token).error.empty());
}

TEST_CASE("preview: the provider for a type") {
    const auto mime = MimeDatabase::load();
    const TempDir dir;
    const rummage::core::CancelToken token;
    const auto content_of = [&](const char* type, const fs::path& path) {
        return preview::provider_for(*mime, mime->find(type))
            .load({.path = path, .mime = type, .width = 0, .height = 0}, token);
    };
    const fs::path json = write(dir.path / "a.json", "{\n\t\"a\": 1\n}\n");
    const preview::Content text = content_of("application/json", json);
    REQUIRE(std::holds_alternative<preview::Text>(text));
    CHECK(std::get<preview::Text>(text).lines.size() == 3);
    // Text by name, binary inside: the information card.
    const fs::path fake = write(dir.path / "fake.txt", std::string{"\0\0", 2});
    CHECK(std::holds_alternative<preview::Info>(content_of("text/plain", fake)));
    // A text file that cannot be read: the card, saying why.
    const preview::Content missing = content_of("text/plain", dir.path / "missing.txt");
    REQUIRE(std::holds_alternative<preview::Info>(missing));
    CHECK_FALSE(std::get<preview::Info>(missing).error.empty());
    CHECK(std::holds_alternative<preview::Info>(content_of("application/zip", json)));
    CHECK(std::holds_alternative<preview::Info>(content_of("inode/directory", dir.path)));
    CHECK(std::holds_alternative<rummage::picture::Loaded>(
        content_of("image/png", fs::path{RUMMAGE_TEST_DATA} / "image" / "blocks.png")));
    // A type not known yet gets the card until it is.
    CHECK(std::holds_alternative<preview::Info>(
        preview::provider_for(*mime, rummage::fs::kUnknownMime)
            .load({.path = json, .mime = {}, .width = 0, .height = 0}, token)));
}

TEST_CASE("preview: the info pane's summary of a photo, a text file and a folder") {
    const TempDir dir;
    const rummage::core::CancelToken token;
    const preview::Summary photo =
        preview::summarize(fs::path{RUMMAGE_TEST_DATA} / "image" / "shot.jpg", "image/jpeg",
                           preview::Kind::image, token);
    REQUIRE(photo.photo.has_value());
    CHECK(photo.photo.value_or(rummage::picture::PhotoInfo{}).camera == "OM-1");
    CHECK_FALSE(photo.text);
    CHECK_FALSE(photo.info.permissions.empty());

    std::string many;
    for (int i = 0; i < 100; ++i) {
        many += "line\n";
    }
    const preview::Summary text = preview::summarize(write(dir.path / "a.txt", many), "text/plain",
                                                     preview::Kind::text, token);
    const preview::Text lines = text.text.value_or(preview::Text{});
    CHECK(lines.lines.size() == preview::kSummaryLines);
    CHECK(lines.truncated);
    // A text file that cannot be read has no lines; nor has a binary one.
    CHECK_FALSE(
        preview::summarize(dir.path / "missing.txt", "text/plain", preview::Kind::text, token)
            .text);
    CHECK_FALSE(preview::summarize(write(dir.path / "b.txt", std::string{"a\0b", 3}), "text/plain",
                                   preview::Kind::text, token)
                    .text);
    CHECK_FALSE(text.photo);

    const preview::Summary folder =
        preview::summarize(dir.path, "inode/directory", preview::Kind::info, token);
    CHECK(folder.info.items == std::optional<std::uint64_t>{2});  // a.txt, b.txt
}
