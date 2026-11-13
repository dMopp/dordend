#include "rummage/isolate/helpers.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/formats/image/load.hpp"
#include "rummage/formats/image/photo.hpp"
#include "rummage/formats/text/read.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/text/text.hpp"
#include "protocol.hpp"
#include "temp_dir.hpp"

namespace fs = std::filesystem;
namespace image = rummage::formats::image;
namespace picture = rummage::picture;
namespace isolate = rummage::isolate;
namespace text = rummage::text;
using rummage::test::TempDir;
using picture::Failure;

namespace {

const fs::path kData = fs::path{RUMMAGE_TEST_DATA} / "image";

isolate::HelperOptions real(std::size_t max_helpers = 4) {
    isolate::HelperOptions options;
    options.program = RUMMAGE_PREVIEW_HELPER;
    options.max_helpers = max_helpers;
    return options;
}

isolate::HelperOptions fake(const char* mode) {
    isolate::HelperOptions options;
    options.program = RUMMAGE_FAKE_HELPER;
    options.args = {mode};
    options.max_helpers = 1;
    options.timeout = std::chrono::milliseconds{300};
    return options;
}

picture::LoadRequest request(const fs::path& path, std::uint32_t side = 128) {
    return {.path = path, .max_width = side, .max_height = side};
}

}  // namespace

TEST_CASE("helpers: the helper makes the same pictures as this process") {
    isolate::Helpers helpers{real()};
    for (const char* name : {"blocks.png", "blocks.jpg", "blocks.gif", "blocks.webp", "blocks.jxl",
                             "blocks-orient6.jpg", "shot.jpg", "with-thumbnail.jpg",
                             "tiff-preview.tif", "truncated.jpg"}) {
        INFO(name);
        const picture::Loaded there = helpers.load(request(kData / name));
        const picture::Loaded here = image::load(request(kData / name));
        CHECK(there.failure == here.failure);
        CHECK(there.detail == here.detail);
        CHECK(there.from_preview == here.from_preview);
        CHECK(there.full_width == here.full_width);
        CHECK(there.full_height == here.full_height);
        CHECK(there.image.width == here.image.width);
        CHECK(there.image.rgba == here.image.rgba);
    }
    CHECK(helpers.running() == 1);  // one after the other: one helper did it all
}

TEST_CASE("helpers: a photo's information from the helper") {
    isolate::Helpers helpers{real(1)};
    for (const char* name : {"shot.jpg", "with-thumbnail.jpg", "blocks.png", "truncated.jpg"}) {
        INFO(name);
        const picture::PhotoInfo there = helpers.photo_info(kData / name);
        const picture::PhotoInfo here = image::read_photo_info(kData / name);
        CHECK(there.width == here.width);
        CHECK(there.height == here.height);
        CHECK(there.camera == here.camera);
        CHECK(there.lens == here.lens);
        CHECK(there.exposure_time == here.exposure_time);
        CHECK(there.aperture == here.aperture);
        CHECK(there.iso == here.iso);
        CHECK(there.taken == here.taken);
        CHECK(there.software == here.software);
        CHECK(there.rating == here.rating);
    }
    const picture::PhotoInfo shot = helpers.photo_info(kData / "shot.jpg");
    CHECK(shot.camera == "OM-1");
    CHECK_FALSE(shot.rating);  // shot.jpg.xmp is not read (2e-4)
    CHECK(helpers.photo_info(kData / "missing.jpg").camera.empty());
}

TEST_CASE("helpers: a photo's information travels whole, and nonsense is refused") {
    picture::PhotoInfo info;
    info.width = 5220;
    info.height = 3912;
    info.camera = "OM-1";
    info.lens = std::string(400, 'L');  // cut at 255 bytes
    info.rating = -1;
    const std::vector<std::byte> bytes = isolate::protocol::encode_info(info);
    const std::optional<picture::PhotoInfo> decoded = isolate::protocol::decode_info(bytes);
    REQUIRE(decoded.has_value());
    const picture::PhotoInfo back = decoded.value_or(picture::PhotoInfo{});
    CHECK(back.width == 5220);
    CHECK(back.camera == "OM-1");
    CHECK(back.lens == std::string(255, 'L'));
    CHECK(back.rating == -1);
    CHECK_FALSE(isolate::protocol::decode_info(std::span{bytes}.first(bytes.size() - 1)));
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(isolate::protocol::decode_info(longer));
    CHECK_FALSE(isolate::protocol::decode_info({}));
}

TEST_CASE("helpers: never more helpers than allowed, every request answered") {
    isolate::Helpers helpers{real(2)};
    std::vector<std::future<picture::Loaded>> results;
    results.reserve(12);
    for (int i = 0; i < 12; ++i) {
        results.push_back(std::async(std::launch::async, [&helpers] {
            return helpers.load(request(kData / "shot.jpg", 256));
        }));
    }
    for (std::future<picture::Loaded>& result : results) {
        const picture::Loaded got = result.get();
        INFO(got.detail);
        CHECK(got.failure == Failure::none);
    }
    CHECK(helpers.running() <= 2);
}

TEST_CASE("helpers: an idle helper is closed and waited for, then replaced unseen") {
    isolate::HelperOptions options = real(1);
    options.idle = std::chrono::milliseconds{50};
    isolate::Helpers helpers{options};
    CHECK(helpers.load(request(kData / "blocks.png")).failure == Failure::none);
    CHECK(helpers.running() == 1);
    std::this_thread::sleep_for(std::chrono::milliseconds{300});
    CHECK(helpers.running() == 0);  // closed by rummage and waited for: no zombie
    CHECK(helpers.load(request(kData / "blocks.png")).failure == Failure::none);
    // A goodbye racing with a request: sent again to a new one (which says goodbye too here).
    isolate::Helpers leaving{fake("goodbye")};
    const picture::Loaded left = leaving.load(request(kData / "blocks.png"));
    CHECK(left.failure == Failure::unavailable);
    CHECK(leaving.running() == 0);
}

TEST_CASE("helpers: a helper that crashes, hangs or talks nonsense is let go of") {
    for (const char* mode : {"crash", "hang", "garbage", "big-lie"}) {
        INFO(mode);
        isolate::Helpers helpers{fake(mode)};
        const auto start = std::chrono::steady_clock::now();
        const picture::Loaded loaded = helpers.load(request(kData / "blocks.png"));
        CHECK(loaded.failure == Failure::unavailable);
        CHECK(loaded.error() == "Preview unavailable");
        CHECK_FALSE(loaded.detail.empty());
        CHECK(loaded.image.empty());
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});
        CHECK(helpers.running() == 0);  // killed and waited for
    }
}

TEST_CASE("helpers: a helper without its cage never works, and rummage says why") {
    isolate::Helpers helpers{fake("no-sandbox")};
    const picture::Loaded loaded = helpers.load(request(kData / "blocks.png"));
    CHECK(loaded.failure == Failure::unavailable);
    CHECK(loaded.detail == "the preview helper could not set up its sandbox");
    CHECK(helpers.running() == 0);
}

TEST_CASE("helpers: no helper, no picture, and nothing parsed here instead") {
    isolate::HelperOptions missing;
    missing.program = "/nonexistent/rummage-preview";
    isolate::Helpers helpers{missing};
    const picture::Loaded loaded = helpers.load(request(kData / "blocks.png"));
    CHECK(loaded.failure == Failure::unavailable);
    CHECK(loaded.image.empty());
    CHECK(helpers.running() == 0);
}

TEST_CASE("helpers: only regular files are sent") {
    const TempDir dir;
    isolate::Helpers helpers{real(1)};
    const fs::path fifo = dir.path / "pipe.jpg";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(helpers.load(request(fifo)).failure == Failure::unreadable);  // and returns at once
    CHECK(helpers.load(request(dir.path)).failure == Failure::unreadable);
    CHECK(helpers.load(request(dir.path / "missing.png")).failure == Failure::unreadable);
    CHECK(helpers.running() == 0);
}

TEST_CASE("helpers: load_picture goes where the loader says") {
    auto helpers = std::make_shared<isolate::Helpers>(fake("crash"));
    picture::set_loader([helpers](const picture::LoadRequest& r) { return helpers->load(r); });
    CHECK(picture::load_picture(request(kData / "blocks.png")).failure == Failure::unavailable);
    // No loader: no picture, never made here instead.
    picture::set_loader({});
    CHECK(picture::load_picture(request(kData / "blocks.png")).failure == Failure::unavailable);
    // Back to what the other tests use: the image family in this process.
    picture::set_loader([](const picture::LoadRequest& r) { return image::load(r); });
    CHECK(picture::load_picture(request(kData / "blocks.png")).failure == Failure::none);
}

TEST_CASE("helpers: the helper reads the same text as this process") {
    const TempDir dir;
    const auto write = [&](const char* name, std::string_view bytes) {
        const fs::path path = dir.path / name;
        std::ofstream{path, std::ios::binary} << bytes;
        return path;
    };
    std::string many;
    for (int i = 0; i < 3000; ++i) {
        many += "line\t" + std::to_string(i) + (i % 2 == 0 ? "\r\n" : "\n");
    }
    const std::vector<fs::path> files{
        write("utf8.txt", "caffè ☕\n\tok\n"),
        write("latin1.txt", "caff\xE8 3\x80\n"),
        write("utf16.txt", std::string_view{"\xFF\xFEh\0i\0\n\0\xE8\0", 10}),
        write("binary.txt", std::string_view{"a\0b", 3}),
        write("empty.txt", ""),
        write("newline.txt", "\n"),
        write("many.txt", many),
        fs::path{RUMMAGE_TEST_DATA} / ".." / "text_test.cpp",
    };
    isolate::Helpers helpers{real(1)};
    for (const text::Limits limits :
         {text::Limits{}, text::Limits{.max_bytes = 8192, .max_line_bytes = 256, .max_lines = 40},
          text::Limits{.max_bytes = 100, .max_line_bytes = 3, .max_lines = 0, .tab_width = 8},
          text::Limits{.max_bytes = 8192, .tail = true}}) {
        for (const fs::path& file : files) {
            INFO(file.filename().string(), " ", limits.max_bytes);
            const bool code = file.extension() == ".cpp";
            const text::LoadRequest request{
                .path = file, .mime = code ? "text/x-c++src" : "text/plain", .limits = limits};
            const text::Loaded there = helpers.load_text(request);
            const text::Loaded here = rummage::formats::text::load(request);
            CHECK(there.failure == here.failure);
            CHECK(there.detail == here.detail);
            CHECK(there.text.lines == here.text.lines);
            CHECK(there.text.truncated == here.text.truncated);
            CHECK(there.text.encoding == here.text.encoding);
            CHECK(there.text.crlf == here.text.crlf);
            CHECK(there.text.tail == here.text.tail);
            CHECK(there.text.language == here.text.language);
            CHECK(there.text.spans == here.text.spans);
            CHECK(there.text.language == (code ? "C++" : ""));
            CHECK(there.text.spans.empty() != code);
        }
    }
    CHECK(helpers.load_text({.path = files[2], .mime = {}, .limits = {}}).text.encoding ==
          text::Encoding::utf16le);
    CHECK(helpers.load_text({.path = files[3], .mime = {}, .limits = {}}).failure ==
          text::Failure::binary);
    CHECK(helpers.running() == 1);
    // Only regular files are sent.
    const fs::path fifo = dir.path / "pipe.txt";
    REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
    CHECK(helpers.load_text({.path = fifo, .mime = {}, .limits = {}}).failure ==
          text::Failure::unreadable);
    CHECK(helpers.load_text({.path = dir.path / "missing.txt", .mime = {}, .limits = {}}).failure ==
          text::Failure::unreadable);
}

TEST_CASE("helpers: a text request travels whole, and too much is refused") {
    namespace protocol = isolate::protocol;
    const std::vector<std::byte> bytes = protocol::encode_text_request(
        {.max_bytes = 8192, .max_line_bytes = 256, .max_lines = 40, .tab_width = 8}, "notes.md",
        "text/markdown");
    const std::optional<protocol::TextAsk> ask = protocol::decode_text_request(bytes);
    REQUIRE(ask.has_value());
    const protocol::TextAsk back = ask.value_or(protocol::TextAsk{});
    CHECK(back.limits.max_bytes == 8192);
    CHECK(back.limits.max_line_bytes == 256);
    CHECK(back.limits.max_lines == 40);
    CHECK(back.limits.tab_width == 8);
    CHECK(back.name == "notes.md");
    CHECK(back.mime == "text/markdown");
    CHECK_FALSE(back.limits.tail);
    const std::optional<protocol::TextAsk> tail = protocol::decode_text_request(
        protocol::encode_text_request({.tail = true}, "app.log", "text/x-log"));
    CHECK(tail.value_or(protocol::TextAsk{}).limits.tail);
    // A tail that is neither 0 nor 1 is nonsense.
    std::vector<std::byte> two = bytes;
    two[offsetof(protocol::TextRequest, tail)] = std::byte{2};
    CHECK_FALSE(protocol::decode_text_request(two));
    // Names are cut at 255 bytes; limits past the helper's are lowered to them.
    const std::optional<protocol::TextAsk> long_name = protocol::decode_text_request(
        protocol::encode_text_request({.max_bytes = std::size_t{1} << 30U,
                                       .max_line_bytes = 1U << 20U,
                                       .max_lines = 0,
                                       .tab_width = 100},
                                      std::string(400, 'n'), "text/plain"));
    REQUIRE(long_name.has_value());
    CHECK(long_name.value_or(protocol::TextAsk{}).name.size() == 255);
    CHECK(long_name.value_or(protocol::TextAsk{}).limits.max_bytes == protocol::kMaxTextBytes);
    CHECK(long_name.value_or(protocol::TextAsk{}).limits.tab_width == protocol::kMaxTabWidth);
    // Nonsense: cut short, longer, zero limits.
    CHECK_FALSE(protocol::decode_text_request(std::span{bytes}.first(bytes.size() - 1)));
    std::vector<std::byte> longer = bytes;
    longer.push_back(std::byte{0});
    CHECK_FALSE(protocol::decode_text_request(longer));
    CHECK_FALSE(
        protocol::decode_text_request(protocol::encode_text_request({.max_bytes = 0}, "a", "b")));
    CHECK_FALSE(protocol::decode_text_request(
        protocol::encode_text_request({.max_line_bytes = 0}, "a", "b")));
    CHECK_FALSE(
        protocol::decode_text_request(protocol::encode_text_request({.tab_width = 0}, "a", "b")));
    CHECK_FALSE(protocol::decode_text_request({}));
}

TEST_CASE("helpers: a helper that lies about a text is let go of") {
    const TempDir dir;
    const fs::path file = dir.path / "a.txt";
    std::ofstream{file} << "a\nb\nc\n";
    for (const char* mode : {"crash", "garbage", "text-lie"}) {
        INFO(mode);
        isolate::Helpers helpers{fake(mode)};
        const text::Loaded loaded = helpers.load_text({.path = file, .mime = {}, .limits = {}});
        CHECK(loaded.failure == text::Failure::unavailable);
        CHECK_FALSE(loaded.detail.empty());
        CHECK(loaded.text.lines.empty());
        CHECK(helpers.running() == 0);
    }
}

TEST_CASE("helpers: a text's payload travels whole, and spans out of place are refused") {
    namespace protocol = isolate::protocol;
    text::Text sent;
    sent.lines = {"int x;", "", "// é"};
    sent.spans = {{{.start = 0, .length = 3, .style = text::Style::type}},
                  {},
                  {{.start = 0, .length = 5, .style = text::Style::comment}}};
    std::uint32_t bytes = 0;
    std::uint32_t spans = 0;
    const std::vector<std::byte> payload = protocol::encode_text_payload(sent, bytes, spans);
    CHECK(bytes == 13);
    CHECK(spans == 2);
    CHECK(payload.size() == 16 + (2 * 12));
    text::Text back;
    REQUIRE(protocol::decode_text_payload(payload, 3, bytes, spans, true, {}, back));
    CHECK(back.lines == sent.lines);
    CHECK(back.spans == sent.spans);
    // Without colours: no spans at all, not one empty list per line.
    text::Text plain;
    plain.lines = sent.lines;
    const std::vector<std::byte> plain_payload = protocol::encode_text_payload(plain, bytes, spans);
    REQUIRE(protocol::decode_text_payload(plain_payload, 3, bytes, 0, false, {}, back));
    CHECK(back.spans.empty());

    // A span changed in the payload: past its line, overlapping, out of order, an unknown style.
    const auto refused = [&](std::size_t number, std::uint32_t value) {
        std::vector<std::byte> changed = payload;
        std::memcpy(changed.data() + 16 + (number * 4), &value, sizeof(value));
        text::Text out;
        return !protocol::decode_text_payload(changed, 3, 13, 2, true, {}, out);
    };
    CHECK(refused(0, 3));                // the first span on a line that is not there
    CHECK(refused(1, 4));                // starts at 4, 3 long: past "int x;"
    CHECK(refused(2, (7U << 8U) | 1U));  // 7 long
    CHECK(refused(2, (3U << 8U) | 8U));  // style 8
    CHECK(refused(2, 1U));               // 0 long
    CHECK(refused(3, 0));                // the second span over the first
    CHECK_FALSE(refused(1, 0));          // unchanged
    text::Text out;
    CHECK_FALSE(protocol::decode_text_payload(std::span{payload}.first(payload.size() - 1), 3, 13,
                                              2, true, {}, out));
    CHECK_FALSE(protocol::decode_text_payload(payload, 2, 13, 2, true, {}, out));  // lines
    CHECK_FALSE(protocol::decode_text_payload(payload, 3, 13, 2, true, {.max_line_bytes = 5}, out));
}
