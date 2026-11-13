#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <filesystem>
#include <memory>

#include <doctest/doctest.h>

#include "rummage/engines/engines.hpp"
#include "rummage/isolate/helpers.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/preview/preview.hpp"
#include "rummage/thumbs/cache.hpp"
#include "temp_dir.hpp"
namespace fs = std::filesystem;
namespace isolate = rummage::isolate;
namespace picture = rummage::picture;
namespace engines = rummage::engines;
using rummage::test::TempDir;
static const fs::path fixtures{RUMMAGE_PDF_DATA};

TEST_CASE("pdf: actual sandbox draws the first page, rotated pages and built-in fonts") {
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    auto red = helpers.load({.path = fixtures / "red.pdf", .max_width = 128, .max_height = 128});
    INFO(red.detail);
    REQUIRE(red.failure == picture::Failure::none);
    CHECK(red.image.width == 128);
    CHECK(red.image.height == 64);
    CHECK(red.image.rgba[0] > 220);
    CHECK(red.image.rgba[1] < 30);
    CHECK(red.image.rgba[3] == 255);

    auto rotated =
        helpers.load({.path = fixtures / "rotated.pdf", .max_width = 128, .max_height = 128});
    REQUIRE(rotated.failure == picture::Failure::none);
    CHECK(rotated.image.width == 64);
    CHECK(rotated.image.height == 128);

    // Helvetica is not embedded: MuPDF's own font draws it, without opening a file in the cage.
    auto text = helpers.load({.path = fixtures / "text.pdf", .max_width = 200, .max_height = 200});
    REQUIRE(text.failure == picture::Failure::none);
    std::size_t ink = 0;
    for (std::size_t i = 0; i < text.image.rgba.size(); i += 4) {
        ink += text.image.rgba[i] < 100;
    }
    CHECK(text.image.rgba[0] == 255);  // white paper
    CHECK(ink > 200);

    auto large =
        helpers.load({.path = fixtures / "red.pdf", .max_width = 8000, .max_height = 8000});
    REQUIRE(large.failure == picture::Failure::none);
    CHECK(large.image.width <= 2048);
}

TEST_CASE("pdf: damaged files fail without killing the helper") {
    TempDir dir;
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    for (const auto& path :
         {dir.write("broken.pdf", "not a pdf"),
          dir.write("cut.pdf", "%PDF-1.4\n1 0 obj<</Type/Catalog"), dir.write("empty.pdf", "")}) {
        INFO(path);
        CHECK(helpers.load({.path = path, .max_width = 128, .max_height = 128}).failure !=
              picture::Failure::none);
    }
    CHECK(
        helpers.load({.path = fixtures / "red.pdf", .max_width = 128, .max_height = 128}).failure ==
        picture::Failure::none);
}

TEST_CASE("pdf: MIME routes to its own helper, thumbnails and Quick Look use it") {
    TempDir dir;
    const auto source = dir.path / "a 'paper'; $test.pdf";
    fs::copy_file(fixtures / "red.pdf", source);
    const auto registry = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return fs::path{name == "rummage-pdf" ? RUMMAGE_PDF_HELPER : RUMMAGE_BASE_HELPER};
    });
    isolate::use_engines(registry);
    // What rummage does at start-up: Quick Look asks the installed registry who shows a type.
    engines::set_installed(std::make_shared<const engines::Registry>(registry));
    const rummage::thumbs::Cache cache{dir.path / "cache", "test"};
    const auto generated = cache.get(source, rummage::thumbs::Size::normal, "application/pdf");
    INFO(generated.error);
    REQUIRE_FALSE(generated.image.empty());
    CHECK(generated.image.rgba[0] > 220);
    const auto mime = rummage::fs::MimeDatabase::load();
    const auto& provider = rummage::preview::provider_for(*mime, mime->find("application/pdf"));
    CHECK(provider.kind() == rummage::preview::Kind::image);
    const auto content =
        provider.load({.path = source, .mime = "application/pdf", .width = 128, .height = 128}, {});
    REQUIRE(std::holds_alternative<picture::Loaded>(content));
    CHECK(std::get<picture::Loaded>(content).failure == picture::Failure::none);
}
