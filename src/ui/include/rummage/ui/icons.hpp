#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "rummage/ui/color.hpp"
#include "rummage/ui/icon_theme.hpp"
#include "rummage/ui/renderer.hpp"
#include "rummage/ui/text.hpp"
#include "rummage/ui/texture.hpp"

namespace rummage::ui {

// Tabler line icons drawn from an icon font by glyph name ("folder", "photo"...), tinted with
// any color. The fonts are compiled into the program (third_party/tabler-icons, only the icons
// rummage uses): the outline icons, and the filled ones asked for with a "-filled" suffix
// ("trash-filled"). Without FreeType able to load them, draw() draws nothing and available() is
// false.
//
// Not thread-safe; uses the FontSystem it was made with, which must outlive it.
class IconSet {
public:
    explicit IconSet(FontSystem& fonts);

    // "system" reads desktop settings, "builtin" uses Tabler, otherwise an installed theme.
    // Applied at the next draw, when a GL context is current.
    void set_theme(std::string theme);
    void set_place_icons(std::map<std::string, std::string> icons);

    [[nodiscard]] const std::string& selected_theme() const noexcept { return selected_theme_; }

    [[nodiscard]] bool available() const noexcept { return outline_.has_value(); }

    // True when the fonts have an icon of that name.
    [[nodiscard]] bool has(std::string_view name);

    // Draws the icon centered in the size x size box at (x, y), logical pixels. An unknown name
    // draws the "file" icon and is logged once, so a typo shows up as a wrong icon rather than a
    // hole.
    void draw(Renderer& renderer, std::string_view name, float x, float y, float size, Color color);
    // A folder's preferred theme name/local path is tried before its MIME and built-in icons.
    void draw_file(Renderer& renderer, std::string_view mime, std::string_view fallback, float x,
                   float y, float size, Color color, std::string_view preferred = {});

    // Overrides use theme names, builtin:<glyph>, or absolute SVG/PNG paths. Missing assets
    // use the normal icon; trash_full inherits trash when no separate override is configured.
    void draw_place(Renderer& renderer, std::string_view key, std::string_view fallback, float x,
                    float y, float size, Color color);

private:
    struct Glyph {
        std::uint32_t font{0};
        std::uint32_t index{0};  // 0: none
    };

    Glyph glyph(std::string_view name);
    void draw_builtin(Renderer& renderer, std::string_view name, float x, float y, float size,
                      Color color);
    bool draw_themed(Renderer& renderer, std::string_view name, float x, float y, float size,
                     Color color);

    FontSystem& fonts_;
    std::optional<std::uint32_t> outline_;
    std::optional<std::uint32_t> filled_;
    std::unordered_map<std::string, Glyph> glyphs_;
    std::map<std::string, std::string> place_icons_;
    std::optional<std::string> pending_theme_;
    std::string selected_theme_{"builtin"};
    std::unique_ptr<IconTheme> theme_;
    std::unordered_map<std::string, std::unique_ptr<Texture>> themed_;
    std::size_t theme_bytes_{0};
    std::unordered_set<std::string> svg_warnings_;
};

}  // namespace rummage::ui
