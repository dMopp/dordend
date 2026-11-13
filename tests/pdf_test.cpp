#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
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

TEST_CASE("pdf: password protected files say so, an empty user password just opens") {
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    auto locked =
        helpers.load({.path = fixtures / "locked.pdf", .max_width = 128, .max_height = 128});
    CHECK(locked.failure == picture::Failure::locked);
    CHECK(locked.error() == "This file is password protected");
    auto open =
        helpers.load({.path = fixtures / "owner-only.pdf", .max_width = 128, .max_height = 128});
    INFO(open.detail);
    REQUIRE(open.failure == picture::Failure::none);
    CHECK(open.image.rgba[0] > 220);
}

TEST_CASE("pdf: the cache remembers why a PDF failed, for the icon's mark") {
    TempDir dir;
    const auto registry = engines::Registry::load(RUMMAGE_ENGINE_DATA, [](std::string_view name) {
        return fs::path{name == "rummage-pdf" ? RUMMAGE_PDF_HELPER : RUMMAGE_BASE_HELPER};
    });
    isolate::use_engines(registry);
    const rummage::thumbs::Cache cache{dir.path / "cache", "test"};
    const auto locked = dir.path / "locked.pdf";
    fs::copy_file(fixtures / "locked.pdf", locked);
    const auto broken = dir.write("broken.pdf", "%PDF-1.4\nnothing else");
    for (int round = 0; round < 2; ++round) {
        INFO("round " << round);  // the second time from fail/, without the helper
        const auto first = cache.get(locked, rummage::thumbs::Size::normal, "application/pdf");
        CHECK(first.image.empty());
        CHECK(first.failure == picture::Failure::locked);
        const auto second = cache.get(broken, rummage::thumbs::Size::normal, "application/pdf");
        CHECK(second.image.empty());
        CHECK(second.failure == picture::Failure::damaged);
    }
    CHECK(fs::exists(dir.path / "cache/fail/test-document-v1"));
    // The reason is kept per file: the file changed, the mark goes.
    fs::copy_file(fixtures / "red.pdf", locked, fs::copy_options::overwrite_existing);
    fs::last_write_time(locked, fs::last_write_time(locked) + std::chrono::seconds{5});
    const auto fixed = cache.get(locked, rummage::thumbs::Size::normal, "application/pdf");
    CHECK_FALSE(fixed.image.empty());
    CHECK(fixed.failure == picture::Failure::none);
}

// Security review: hostile files. Each must end in bounded time, never take the app down, and
// leave the helper pool working for the next file.
namespace {
const fs::path attacks = fixtures / "attack";

struct Timed {
    picture::Loaded loaded;
    double seconds{0.0};
};

Timed timed_load(isolate::Helpers& helpers, const fs::path& path) {
    const auto start = std::chrono::steady_clock::now();
    Timed result{.loaded = helpers.load({.path = path, .max_width = 256, .max_height = 256})};
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return result;
}

void still_works(isolate::Helpers& helpers) {
    const auto red =
        helpers.load({.path = fixtures / "red.pdf", .max_width = 64, .max_height = 64});
    CHECK(red.failure == picture::Failure::none);
}
}  // namespace

TEST_CASE("pdf security: decompression bombs hit the memory budget, quickly") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    for (const char* name : {"bomb-font.pdf", "bomb-image.pdf"}) {
        INFO(name);
        const Timed run = timed_load(helpers, attacks / name);
        INFO(run.loaded.detail << ", " << run.seconds << " s");
        // Not killed by the timeout: the helper itself stopped the bomb.
        CHECK(run.loaded.failure != picture::Failure::unavailable);
        CHECK(run.seconds < 7.0);
        still_works(helpers);
    }
}

TEST_CASE("pdf security: endless drawing is cut off by the helper timeout") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    const Timed run = timed_load(helpers, attacks / "bomb-cpu.pdf");
    INFO(run.loaded.detail << ", " << run.seconds << " s");
    CHECK(run.seconds < 10.0);
    still_works(helpers);
}

TEST_CASE("pdf security: JavaScript never runs, external stream files are never read") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    const Timed script = timed_load(helpers, attacks / "javascript.pdf");
    INFO(script.loaded.detail);
    REQUIRE(script.loaded.failure == picture::Failure::none);  // while(true){} did not run
    CHECK(script.seconds < 2.0);
    CHECK(script.loaded.image.rgba[0] > 220);

    const Timed external = timed_load(helpers, attacks / "external.pdf");
    INFO(external.loaded.detail);
    if (external.loaded.failure == picture::Failure::none) {
        // The page's only content is /F (/etc/passwd): nothing of it is drawn.
        for (std::size_t i = 0; i < external.loaded.image.rgba.size(); i += 4) {
            REQUIRE(external.loaded.image.rgba[i] == 255);
            REQUIRE(external.loaded.image.rgba[i + 1] == 255);
        }
    }
    still_works(helpers);
}

TEST_CASE("pdf security: recursion, deep nesting and absurd pages end safely") {
    isolate::Helpers helpers{
        {.program = RUMMAGE_PDF_HELPER, .args = {}, .timeout = std::chrono::seconds{8}}};
    for (const char* name : {"recursive-form.pdf", "recursive-pages.pdf", "deep.pdf",
                             "huge-page.pdf", "tiny-page.pdf", "nan-page.pdf", "no-pages.pdf",
                             "lying-count.pdf"}) {
        INFO(name);
        const Timed run = timed_load(helpers, attacks / name);
        INFO(run.loaded.detail << ", " << run.seconds << " s");
        CHECK(run.seconds < 7.0);
        CHECK(run.loaded.failure != picture::Failure::unavailable);  // the helper did not die
        if (run.loaded.failure == picture::Failure::none) {
            CHECK(run.loaded.image.width <= 256);
            CHECK(run.loaded.image.height <= 256);
            CHECK(run.loaded.image.rgba.size() ==
                  std::size_t{run.loaded.image.width} * run.loaded.image.height * 4);
        }
        still_works(helpers);
    }
    CHECK(timed_load(helpers, attacks / "no-pages.pdf").loaded.failure ==
          picture::Failure::damaged);
    // A page tree that claims two billion pages is refused as damaged, not walked.
    CHECK(timed_load(helpers, attacks / "lying-count.pdf").loaded.failure ==
          picture::Failure::damaged);
}

TEST_CASE("pdf security: only PDF is parsed, never MuPDF's other formats") {
    isolate::Helpers helpers{{.program = RUMMAGE_PDF_HELPER, .args = {}}};
    for (const char* name : {"cbz-named.pdf", "svg-named.pdf", "html-named.pdf"}) {
        INFO(name);
        const Timed run = timed_load(helpers, attacks / name);
        INFO(run.loaded.detail);
        CHECK(run.loaded.failure == picture::Failure::damaged);
    }
    still_works(helpers);
}
