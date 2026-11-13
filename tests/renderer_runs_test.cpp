#include <GLES3/gl3.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/ui/color.hpp"
#include "rummage/ui/icons.hpp"
#include "rummage/ui/offscreen.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/views/confirm_dialog.hpp"
#include "rummage/views/error_dialog.hpp"
#include "temp_dir.hpp"

namespace ui = rummage::ui;

namespace {

constexpr std::int32_t kWidth = 240;
constexpr std::int32_t kHeight = 40;

std::unique_ptr<ui::Offscreen> offscreen(std::int32_t width = kWidth,
                                         std::int32_t height = kHeight) {
    try {
        return std::make_unique<ui::Offscreen>(width, height);
    } catch (const std::runtime_error& error) {
        MESSAGE("no offscreen OpenGL: " << error.what());
        return nullptr;
    }
}

}  // namespace

TEST_CASE("renderer: a line in several colours, by the bytes each glyph comes from") {
    const std::unique_ptr<ui::Offscreen> gl = offscreen();
    if (!gl) {
        return;
    }
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    renderer.begin_frame(kWidth, kHeight, 1.0F, ui::Color::rgb(0x000000));
    // "MMMM" white, "éééé" (two bytes each: bytes 4 to 12) red.
    const std::array runs{ui::ColorRun{.start = 4, .end = 12, .color = ui::Color::rgb(0xFF0000)}};
    const float width =
        renderer.draw_text_runs("MMMMéééé", 4.0F, 4.0F, 24.0F, ui::Color::rgb(0xFFFFFF), runs, 0.0F,
                                ui::Ellipsis::end, ui::Typeface::monospace);
    renderer.end_frame();
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // Monospace: the colour changes halfway along the text.
    const float half = 4.0F + (width / 2.0F);
    int white_left = 0;
    int white_right = 0;
    int red_left = 0;
    int red_right = 0;
    for (std::int32_t y = 0; y < kHeight; ++y) {
        for (std::int32_t x = 0; x < kWidth; ++x) {
            const std::size_t at =
                ((static_cast<std::size_t>(y) * kWidth) + static_cast<std::size_t>(x)) * 4;
            const bool white = pixels[at] > 128 && pixels[at + 1] > 128;
            const bool red = pixels[at] > 128 && pixels[at + 1] < 64;
            const bool left = static_cast<float>(x) < half - 2.0F;
            const bool right = static_cast<float>(x) > half + 2.0F;
            white_left += white && left ? 1 : 0;
            white_right += white && right ? 1 : 0;
            red_left += red && left ? 1 : 0;
            red_right += red && right ? 1 : 0;
        }
    }
    CHECK(white_left > 50);
    CHECK(red_right > 50);
    CHECK(white_right == 0);
    CHECK(red_left == 0);
}

TEST_CASE("renderer: background alpha does not accumulate or fade foreground content") {
    for (const float scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        CAPTURE(scale);
        const auto physical = [scale](float value) {
            return static_cast<int>(std::lround(value * scale));
        };
        const auto gl = offscreen(physical(kWidth), physical(kHeight));
        if (!gl) {
            return;
        }
        ui::FontSystem fonts;
        ui::Renderer renderer{fonts};
        const std::array<std::uint8_t, 4> texel{255, 0, 0, 255};
        ui::Texture texture{1, 1, texel};
        for (const float opacity : {0.0F, 0.7F, 1.0F}) {
            CAPTURE(opacity);
            renderer.begin_frame(physical(kWidth), physical(kHeight), scale,
                                 ui::Color::rgb(0xFFFFFF), opacity);
            renderer.fill_background({0, 0, 80, 40}, ui::Color::rgb(0x00FF00));
            renderer.push_clip({0, 0, 40, 40});
            renderer.fill_background({0, 0, 80, 40}, ui::Color::rgb(0x0000FF));
            renderer.pop_clip();
            renderer.draw_image(texture, {80, 0, 40, 40});
            renderer.fill_rect({120, 0, 40, 40}, ui::Color::rgb(0xFF0000));
            renderer.draw_text("M", 165, 0, 32, ui::Color::rgb(0xFFFFFF));
            renderer.end_frame();
            const auto pixel = [&](float x) {
                std::array<std::uint8_t, 4> rgba{};
                glReadPixels(physical(x), physical(20), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                             rgba.data());
                return rgba;
            };
            const auto blue = pixel(20);
            const auto green = pixel(60);
            const auto clear = pixel(230);
            for (const auto rgba : {blue, green, clear}) {
                CHECK(static_cast<float>(rgba[3]) == doctest::Approx(opacity * 255).epsilon(0.005));
            }
            CHECK(blue[0] == 0);
            CHECK(blue[1] == 0);
            CHECK(blue[2] == blue[3]);
            CHECK(green[0] == 0);
            CHECK(green[1] == green[3]);
            CHECK(green[2] == 0);
            CHECK(clear[0] == clear[3]);
            CHECK(clear[1] == clear[3]);
            CHECK(clear[2] == clear[3]);
            CHECK(pixel(100) == texel);
            CHECK(pixel(140) == texel);
            std::vector<std::uint8_t> glyph(static_cast<std::size_t>(physical(40) * physical(40)) *
                                            4);
            glReadPixels(physical(160), 0, physical(40), physical(40), GL_RGBA, GL_UNSIGNED_BYTE,
                         glyph.data());
            int solid_glyph_pixels = 0;
            for (std::size_t i = 0; i < glyph.size(); i += 4) {
                if (glyph[i] == 255 && glyph[i + 3] == 255) {
                    ++solid_glyph_pixels;
                }
            }
            CHECK(solid_glyph_pixels > 20);
            CHECK(glGetError() == GL_NO_ERROR);
        }
    }
}

TEST_CASE("renderer: adjacent translucent panels meet at fractional pixel boundaries") {
    const auto gl = offscreen();
    if (!gl) {
        return;
    }
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    for (const float scale : {1.25F, 1.5F}) {
        CAPTURE(scale);
        renderer.begin_frame(kWidth, kHeight, scale, ui::Color::rgb(0xFF0000), 0.7F);
        renderer.fill_background({0, 0, 39, 30}, ui::Color::rgb(0x00FF00));
        renderer.fill_background({39, 0, 40, 30}, ui::Color::rgb(0x0000FF));
        renderer.end_frame();
        const int boundary = static_cast<int>(std::lround(39 * scale));
        for (int x = boundary - 2; x <= boundary + 2; ++x) {
            std::array<std::uint8_t, 4> rgba{};
            glReadPixels(x, kHeight - 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            CHECK(rgba[0] == 0);  // no uncovered red seam
            CHECK(rgba[3] == doctest::Approx(0.7 * 255).epsilon(0.005));
            CHECK(rgba[x < boundary ? 1 : 2] == rgba[3]);
            CHECK(rgba[x < boundary ? 2 : 1] == 0);
        }
    }
}

TEST_CASE("folder icons: rendered override, missing or broken artwork, and builtin fallback") {
    const auto gl = offscreen(32, 32);
    if (!gl)
        return;
    rummage::test::TempDir dir;
    const auto custom = dir.write("custom.svg",
                                  "<svg width='32' height='32'>"
                                  "<rect width='32' height='32' fill='red'/></svg>");
    const auto broken = dir.write("broken.svg", "not svg");
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    ui::IconSet icons{fonts};
    icons.set_theme("rummage-nonexistent-test-theme");
    const auto draw = [&](std::string_view preferred) {
        renderer.begin_frame(32, 32, 1.0F, ui::Color::rgb(0x000000));
        icons.draw_file(renderer, "inode/directory", "folder", 0, 0, 32, ui::Color::rgb(0xFFFFFF),
                        preferred);
        renderer.end_frame();
        std::array<std::uint8_t, 32 * 32 * 4> pixels{};
        glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
    };
    const auto normal = draw({});
    const auto override = draw(custom.string());
    CHECK(override != normal);
    const auto middle = (16 * 32 + 16) * 4;
    CHECK(override[middle] == 255);
    CHECK(override[middle + 1] == 0);
    CHECK(draw(broken.string()) == normal);
    CHECK(draw((dir.path / "missing.svg").string()) == normal);
    CHECK(draw("rummage-nonexistent-folder-icon") == normal);
    icons.set_theme("builtin");
    const auto builtin = draw({});
    CHECK(draw(custom.string()) == builtin);
}

TEST_CASE("renderer: password dialogs mask Unicode characters and retain editing geometry") {
    const auto gl = offscreen(600, 300);
    if (!gl)
        return;
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    rummage::views::ConfirmDialog dialog{[] {}};
    const auto render = [&](const std::string& password) {
        dialog.open_password("secret.7z", false, [](auto) {});
        dialog.paste(password);
        renderer.begin_frame(600, 300, 1.0F, ui::Color::rgb(0xFFFFFF));
        dialog.draw(renderer, {0, 0, 600, 300}, {});
        renderer.end_frame();
        std::vector<std::uint8_t> pixels(600 * 300 * 4);
        glReadPixels(0, 0, 600, 300, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
    };
    const auto ascii = render("abc");
    CHECK(ascii == render("é🔒字"));  // three bullets, not one per byte
    CHECK(ascii != render("ab"));
    CHECK(ascii != render(""));
}

TEST_CASE(
    "renderer: action errors wrap scroll and copy all details without losing queued failures") {
    const auto gl = offscreen(600, 300);
    if (!gl)
        return;
    ui::FontSystem fonts;
    ui::Renderer renderer{fonts};
    std::string copied;
    rummage::views::ErrorDialog dialog{[] {}, [&](std::string text) { copied = std::move(text); }};
    std::string detail = "Could not extract archive. Install unrar and try again.\n";
    for (unsigned i = 0; i < 60; ++i)
        detail += "Archive: /photos/é🔒/" + std::string(100, 'a') + ".rar\n";
    dialog.push(detail);
    dialog.push("Another archive failed");
    const auto render = [&] {
        renderer.begin_frame(600, 300, 1.0F, ui::Color::rgb(0xFFFFFF));
        dialog.draw(renderer, {0, 0, 600, 300}, {});
        renderer.end_frame();
        std::vector<std::uint8_t> pixels(600 * 300 * 4);
        glReadPixels(0, 0, 600, 300, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return pixels;
    };
    const auto top = render();
    dialog.key({.keysym = XKB_KEY_End, .text = {}});
    CHECK(top != render());
    dialog.key({.keysym = XKB_KEY_c, .text = {}, .modifiers = {.ctrl = true}});
    CHECK(copied == detail);
    CHECK(dialog.pending() == 2);
    dialog.key({.keysym = XKB_KEY_Home, .text = {}});
    CHECK(top == render());
    dialog.key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK(dialog.detail() == "Another archive failed");
    CHECK(top != render());
    dialog.key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK_FALSE(dialog.is_open());
}
