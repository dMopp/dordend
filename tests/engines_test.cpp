#include "rummage/engines/engines.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/fs/mime.hpp"
#include "temp_dir.hpp"

namespace {

namespace engines = rummage::engines;
using rummage::fs::MimeDatabase;

// Every program is "installed".
std::filesystem::path anywhere(std::string_view program) { return std::filesystem::path{program}; }

constexpr std::string_view kEngine = R"([engine]
name = "test"
program = "rummage-test-engine"
protocol = 4
)";

std::string manifest(std::string_view families) {
    return std::string{kEngine} + std::string{families};
}

// Parses text, which must fail; returns why.
std::string refused(const std::string& text) {
    std::string why;
    const std::optional<engines::Manifest> parsed = engines::parse_manifest(text, "test.toml", why);
    CHECK_FALSE(parsed.has_value());
    return why;
}

// picture::may_show's list until 0.4.0, each still shown by the image family.
constexpr std::array<std::string_view, 30> kBefore{"image/png",
                                                   "image/jpeg",
                                                   "image/gif",
                                                   "image/webp",
                                                   "image/jxl",
                                                   "image/tiff",
                                                   "image/x-adobe-dng",
                                                   "image/x-canon-cr2",
                                                   "image/x-canon-cr3",
                                                   "image/x-canon-crw",
                                                   "image/x-dcraw",
                                                   "image/x-fuji-raf",
                                                   "image/x-kodak-dcr",
                                                   "image/x-kodak-k25",
                                                   "image/x-kodak-kdc",
                                                   "image/x-minolta-mrw",
                                                   "image/x-nikon-nef",
                                                   "image/x-nikon-nrw",
                                                   "image/x-olympus-orf",
                                                   "image/x-panasonic-raw",
                                                   "image/x-panasonic-raw2",
                                                   "image/x-panasonic-rw",
                                                   "image/x-panasonic-rw2",
                                                   "image/x-pentax-pef",
                                                   "image/x-portable-graymap",
                                                   "image/x-portable-pixmap",
                                                   "image/x-sigma-x3f",
                                                   "image/x-sony-arw",
                                                   "image/x-sony-sr2",
                                                   "image/x-sony-srf"};

engines::Engine engine(std::string name, std::vector<engines::Family> families) {
    return {.manifest = {.name = std::move(name),
                         .program = "rummage-test-engine",
                         .families = std::move(families)},
            .program = "rummage-test-engine",
            .manifest_path = {}};
}

}  // namespace

TEST_CASE("engines: the base engine shows what may_show listed before, and text") {
    const std::shared_ptr<const engines::Registry> registry = engines::installed();
    REQUIRE(registry->engines().size() == 1);
    const engines::Engine& base = registry->engines().front();
    CHECK(base.manifest.name == "base");
    CHECK(base.manifest.program == "rummage-preview");
    REQUIRE(base.manifest.families.size() == 2);
    const engines::Family& family = base.manifest.families.front();
    CHECK(family.name == "image");
    CHECK(family.shows == engines::Shows::picture);
    CHECK(family.info);
    CHECK(family.thumbnails);
    CHECK_FALSE(family.subclasses);
    CHECK(family.mime.size() == 30);

    const std::shared_ptr<const MimeDatabase> mime = MimeDatabase::load();
    for (const std::string_view type : kBefore) {
        CAPTURE(type);
        const rummage::fs::MimeId id = mime->find(type);
        REQUIRE(id != rummage::fs::kUnknownMime);
        CHECK(registry->find(*mime, id) == &family);
    }
    CHECK(registry->engine_of(family) == &base);
    for (const std::string_view type : {"application/pdf", "image/heic", "application/zip"}) {
        CAPTURE(type);
        CHECK(registry->find(*mime, mime->find(type)) == nullptr);
    }

    // Text: text/plain and every type under it, as the text provider took them before.
    const engines::Family& text = base.manifest.families.back();
    CHECK(text.name == "text");
    CHECK(text.shows == engines::Shows::text);
    CHECK(text.subclasses);
    CHECK_FALSE(text.info);
    CHECK_FALSE(text.thumbnails);
    for (const std::string_view type :
         {"text/plain", "text/x-csrc", "text/x-python", "application/json", "application/toml",
          "text/markdown", "image/svg+xml", "application/x-shellscript"}) {
        CAPTURE(type);
        const rummage::fs::MimeId id = mime->find(type);
        REQUIRE(id != rummage::fs::kUnknownMime);
        CHECK(mime->is_a(id, MimeDatabase::kTextPlain));
        CHECK(registry->find(*mime, id) == &text);
    }
    CHECK(registry->engine_of(text) == &base);
}

TEST_CASE("engines: a manifest is read whole") {
    std::string why;
    const std::optional<engines::Manifest> parsed = engines::parse_manifest(manifest(R"(
[[family]]
name = "pictures"
shows = "picture"
thumbnails = true
mime = ["image/png", "image/x-test"]

[[family]]
name = "more"
shows = "text"
info = true
subclasses = true
mime = ["text/plain"]
)"),
                                                                            "test.toml", why);
    REQUIRE_MESSAGE(parsed.has_value(), why);
    const engines::Manifest read = parsed.value_or(engines::Manifest{});
    CHECK(read.name == "test");
    CHECK(read.program == "rummage-test-engine");
    REQUIRE(read.families.size() == 2);
    CHECK(read.families[0].name == "pictures");
    CHECK(read.families[0].thumbnails);
    CHECK_FALSE(read.families[0].info);
    CHECK_FALSE(read.families[0].subclasses);
    CHECK(read.families[0].mime == std::vector<std::string>{"image/png", "image/x-test"});
    CHECK(read.families[0].shows == engines::Shows::picture);
    CHECK(read.families[1].shows == engines::Shows::text);
    CHECK(read.families[1].info);
    CHECK(read.families[1].subclasses);
}

TEST_CASE("engines: a manifest rummage cannot trust is refused, saying where") {
    const std::string family =
        "\n[[family]]\nname = \"f\"\nshows = \"picture\"\nmime = [\"a/b\"]\n";
    CHECK(refused("[engine\n").starts_with("test.toml:1: "));
    CHECK(refused(manifest("")).find("no 'family'") != std::string::npos);
    CHECK(refused(std::string{kEngine} + "colour = 1\n" + family).find("unknown key 'colour'") !=
          std::string::npos);
    CHECK(refused(manifest(family + "thumbnail = true\n")).find("unknown key 'thumbnail'") !=
          std::string::npos);
    // Another protocol, a path for a program, a name that is not one.
    CHECK(refused("[engine]\nname = \"x\"\nprogram = \"p\"\nprotocol = 1\n" + family)
              .find("protocol 1, rummage speaks 4") != std::string::npos);
    CHECK(refused("[engine]\nname = \"x\"\nprotocol = 4\nprogram = \"../../bin/sh\"\n" + family)
              .find("must be a file name") != std::string::npos);
    CHECK(refused("[engine]\nname = \"x\"\nprotocol = 4\nprogram = \"..\"\n" + family)
              .find("must be a file name") != std::string::npos);
    CHECK(refused("[engine]\nname = \"Big One\"\nprotocol = 4\nprogram = \"p\"\n" + family)
              .find("is not [a-z0-9-]") != std::string::npos);
    CHECK(refused("[engine]\nname = \"x\"\nprotocol = \"3\"\nprogram = \"p\"\n" + family)
              .find("whole number") != std::string::npos);
    // Families: what they show, their types, booleans.
    CHECK(refused(manifest("[[family]]\nname = \"f\"\nshows = \"video\"\nmime = [\"a/b\"]\n"))
              .find("'shows' must be \"picture\" or \"text\"") != std::string::npos);
    CHECK(refused(manifest("[[family]]\nname = \"f\"\nshows = \"picture\"\nmime = []\n"))
              .find("not empty") != std::string::npos);
    CHECK(refused(manifest("[[family]]\nname = \"f\"\nshows = \"picture\"\nmime = [\"png\"]\n"))
              .find("'png' is not a MIME type") != std::string::npos);
    CHECK(refused(manifest("[[family]]\nname = \"f\"\nshows = \"picture\"\nmime = [\"a/\"]\n"))
              .find("not a MIME type") != std::string::npos);
    CHECK(refused(manifest("[[family]]\nname = \"f\"\nshows = \"picture\"\nmime = [\"a/b\", "
                           "\"a/b\"]\n"))
              .find("listed twice") != std::string::npos);
    CHECK(refused(manifest(family + "info = \"yes\"\n")).find("true or false") !=
          std::string::npos);
    CHECK(refused(manifest(family + family)).find("family 'f' is listed twice") !=
          std::string::npos);
    CHECK(refused(manifest(family + "\n[[family]]\nname = \"g\"\nshows = \"picture\"\nmime = "
                                    "[\"a/b\"]\n"))
              .find("'a/b' is in families 'f' and 'g'") != std::string::npos);
    // The line of the problem.
    CHECK(refused(manifest(family + "info = 3\n")).starts_with("test.toml:10: "));
}

TEST_CASE("engines: exactly the types listed, or their subtypes too") {
    const std::shared_ptr<const MimeDatabase> mime = MimeDatabase::load();
    const rummage::fs::MimeId c_source = mime->find("text/x-csrc");
    const rummage::fs::MimeId plain = mime->find("text/plain");
    REQUIRE(c_source != rummage::fs::kUnknownMime);

    engines::Registry exact;
    REQUIRE(exact.add(engine("a", {{.name = "plain", .mime = {"text/plain"}}})));
    CHECK(exact.find(*mime, plain) != nullptr);
    CHECK(exact.find(*mime, c_source) == nullptr);

    engines::Registry family;
    REQUIRE(
        family.add(engine("a", {{.name = "text", .subclasses = true, .mime = {"text/plain"}}})));
    CHECK(family.find(*mime, plain) != nullptr);
    CHECK(family.find(*mime, c_source) != nullptr);
    CHECK(family.find(*mime, mime->find("image/png")) == nullptr);
    // A type listed exactly wins over a family of subtypes.
    REQUIRE(family.add(engine("b", {{.name = "c", .mime = {"text/x-csrc"}}})));
    CHECK(family.find(*mime, c_source)->name == "c");
    CHECK(family.find(*mime, plain)->name == "text");
}

TEST_CASE("engines: a type stays with the first engine that shows it") {
    engines::Registry registry;
    CHECK(registry.add(engine("a", {{.name = "one", .mime = {"image/png"}}})));
    CHECK_FALSE(registry.add(engine("b", {{.name = "two", .mime = {"image/gif", "image/png"}}})));
    CHECK_FALSE(
        registry.add(engine("c", {{.name = "three", .subclasses = true, .mime = {"image/png"}}})));
    REQUIRE(registry.engines().size() == 1);
    CHECK(registry.engines().front().manifest.name == "a");
}

TEST_CASE("engines: a directory of manifests, the broken ones skipped") {
    const rummage::test::TempDir dir;
    const std::string png =
        "\n[[family]]\nname = \"f\"\nshows = \"picture\"\nmime = [\"image/png\"]\n";
    dir.write("b-second.toml", "[engine]\nname = \"second\"\nprogram = \"two\"\nprotocol = 4\n" +
                                   std::string{"\n[[family]]\nname = \"f\"\nshows = \"picture\"\n"
                                               "mime = [\"image/png\", \"image/gif\"]\n"});
    dir.write("a-first.toml",
              "[engine]\nname = \"first\"\nprogram = \"one\"\nprotocol = 4\n" + png);
    dir.write("c-broken.toml", "[engine]\nname = \"broken\"\n");
    dir.write("d-missing.toml",
              "[engine]\nname = \"missing\"\nprogram = \"absent\"\nprotocol = 4\n" +
                  std::string{"\n[[family]]\nname = \"f\"\nshows = \"picture\"\nmime = "
                              "[\"image/jxl\"]\n"});
    dir.write("e-third.toml", "[engine]\nname = \"third\"\nprogram = \"three\"\nprotocol = 4\n" +
                                  std::string{"\n[[family]]\nname = \"f\"\nshows = \"picture\"\n"
                                              "mime = [\"image/webp\"]\n"});
    dir.write("notes.txt", "not a manifest");

    const engines::Registry registry = engines::Registry::load(dir.path,
                                                               [](std::string_view program) {
                                                                   return program == "absent" ? std::filesystem::path{}
                                       : std::filesystem::path{"/opt"} / program;
                                                               });
    REQUIRE(registry.engines().size() == 2);
    CHECK(registry.engines()[0].manifest.name == "first");
    CHECK(registry.engines()[0].program == "/opt/one");
    CHECK(registry.engines()[0].manifest_path == dir.path / "a-first.toml");
    CHECK(registry.engines()[1].manifest.name == "third");

    CHECK(engines::Registry::load(dir.path / "nothing-here", anywhere).engines().empty());
}

TEST_CASE("engines: a program is a file name, never a path") {
    CHECK(engines::locate_program("").empty());
    CHECK(engines::locate_program("..").empty());
    CHECK(engines::locate_program("../../bin/sh").empty());
    CHECK(engines::locate_program("/bin/sh").empty());
    CHECK(engines::locate_program("rummage-no-such-engine").empty());
    // The tests run from build*/tests: the preview helper is one directory up, not next to them.
    CHECK(engines::locate_program("rummage-preview").empty());
}
