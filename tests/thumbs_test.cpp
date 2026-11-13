#include <chrono>
#include <filesystem>
#include <string>
#include <sys/stat.h>

#include <doctest/doctest.h>

#include "rummage/thumbs/cache.hpp"
#include "rummage/thumbs/md5.hpp"
#include "temp_dir.hpp"

namespace thumbs = rummage::thumbs;
namespace fs = std::filesystem;
using rummage::test::TempDir;

namespace {

const fs::path kData = fs::path{RUMMAGE_TEST_DATA} / "image";

int mode_of(const fs::path& path) {
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    return static_cast<int>(info.st_mode & 0777);
}

}  // namespace

TEST_CASE("thumbs: MD5 of the RFC 1321 test suite") {
    CHECK(thumbs::md5_hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(thumbs::md5_hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(thumbs::md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(thumbs::md5_hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(thumbs::md5_hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(
        thumbs::md5_hex("12345678901234567890123456789012345678901234567890123456789012345678901234"
                        "567890") == "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("thumbs: file URIs are escaped exactly as GLib does") {
    // Expected values from `gio info` (GLib's g_filename_to_uri).
    CHECK(thumbs::file_uri("/d/plain.jpg") == "file:///d/plain.jpg");
    CHECK(thumbs::file_uri("/d/spa ce.jpg") == "file:///d/spa%20ce.jpg");
    CHECK(thumbs::file_uri("/d/semi;colon.jpg") == "file:///d/semi%3Bcolon.jpg");
    CHECK(thumbs::file_uri("/d/excl!amp&'()*+,=:@$~_-.jpg") ==
          "file:///d/excl!amp&'()*+,=:@$~_-.jpg");
    CHECK(thumbs::file_uri("/d/pct%hash#q?.jpg") == "file:///d/pct%25hash%23q%3F.jpg");
    CHECK(thumbs::file_uri("/d/accent\xc3\xb2\xe2\x82\xac.jpg") ==
          "file:///d/accent%C3%B2%E2%82%AC.jpg");
    CHECK(thumbs::file_uri("/d/brack[et]{x}^|`\"<>.jpg") ==
          "file:///d/brack%5Bet%5D%7Bx%7D%5E%7C%60%22%3C%3E.jpg");
    // The spec's own example: the thumbnail name is the MD5 of the URI.
    CHECK(thumbs::md5_hex("file:///home/jens/photos/me.png") == "c6ee772d9e49320e97ec29a7eb5b1697");
}

TEST_CASE("thumbs: sizes") {
    CHECK(thumbs::size_for(1) == thumbs::Size::normal);
    CHECK(thumbs::size_for(128) == thumbs::Size::normal);
    CHECK(thumbs::size_for(129) == thumbs::Size::large);
    CHECK(thumbs::size_for(384) == thumbs::Size::x_large);
    CHECK(thumbs::size_for(5000) == thumbs::Size::xx_large);
    CHECK(thumbs::directory_name(thumbs::Size::x_large) == "x-large");
}

TEST_CASE("thumbs: made once, then read from the cache until the file changes") {
    TempDir dir;
    const fs::path photo = dir.path / "photo one.jpg";
    fs::copy_file(kData / "with-thumbnail.jpg", photo);
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};

    // 480x320 with a 192x128 EXIF thumbnail: enough for 128, so the thumbnail comes from it,
    // scaled to fit, then turned upright (orientation 6).
    const thumbs::Thumbnail made = cache.get(photo, thumbs::Size::normal);
    REQUIRE(made.source == thumbs::Source::preview);
    CHECK(made.image.width == 85);
    CHECK(made.image.height == 128);
    const fs::path stored = cache.path_for(photo, thumbs::Size::normal);
    CHECK(stored ==
          dir.path / "thumbnails" / "normal" / (thumbs::md5_hex(thumbs::file_uri(photo)) + ".png"));
    REQUIRE(fs::exists(stored));
    CHECK(mode_of(stored) == 0600);
    CHECK(mode_of(stored.parent_path()) == 0700);
    CHECK(mode_of(dir.path / "thumbnails") == 0700);

    const thumbs::Thumbnail cached = cache.get(photo, thumbs::Size::normal);
    CHECK(cached.source == thumbs::Source::cache);
    CHECK(cached.image.rgba == made.image.rgba);

    // Changed: the stored one no longer matches its Thumb::MTime.
    fs::last_write_time(photo, fs::last_write_time(photo) + std::chrono::seconds{10});
    CHECK(cache.get(photo, thumbs::Size::normal).source == thumbs::Source::preview);
    CHECK(cache.get(photo, thumbs::Size::normal).source == thumbs::Source::cache);

    // At 256 the embedded 192x128 is too small: the whole image, upright.
    const thumbs::Thumbnail large = cache.get(photo, thumbs::Size::large);
    CHECK(large.source == thumbs::Source::decoded);
    CHECK(large.image.width == 171);
    CHECK(large.image.height == 256);
}

TEST_CASE("thumbs: a failure is remembered until the file changes, make() tries again") {
    TempDir dir;
    const fs::path broken = dir.path / "broken.jpg";
    fs::copy_file(kData / "truncated.jpg", broken);
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};

    const thumbs::Thumbnail first = cache.get(broken, thumbs::Size::large);
    CHECK(first.source == thumbs::Source::failed);
    CHECK(first.image.empty());
    CHECK(fs::exists(dir.path / "thumbnails" / "fail" / "rummage-test" /
                     (thumbs::md5_hex(thumbs::file_uri(broken)) + ".png")));
    CHECK(cache.get(broken, thumbs::Size::large).error == "failed before, and unchanged since");
    CHECK(cache.make(broken, thumbs::Size::large).error != "failed before, and unchanged since");

    // Fixed: a good image under the same name.
    fs::copy_file(kData / "blocks.jpg", broken, fs::copy_options::overwrite_existing);
    fs::last_write_time(broken, fs::last_write_time(broken) + std::chrono::seconds{10});
    CHECK(cache.get(broken, thumbs::Size::large).source == thumbs::Source::decoded);

    CHECK(cache.get(dir.path / "missing.jpg", thumbs::Size::large).source ==
          thumbs::Source::failed);
}

TEST_CASE("thumbs: the cache never thumbnails its own files") {
    TempDir dir;
    const thumbs::Cache cache{dir.path / "thumbnails", "rummage-test"};
    const fs::path photo = dir.path / "a.png";
    fs::copy_file(kData / "blocks.png", photo);
    REQUIRE(cache.get(photo, thumbs::Size::normal).source == thumbs::Source::decoded);
    const thumbs::Thumbnail own =
        cache.get(cache.path_for(photo, thumbs::Size::normal), thumbs::Size::normal);
    CHECK(own.source == thumbs::Source::failed);
}
