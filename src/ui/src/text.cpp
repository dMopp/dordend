#include "nordend/ui/text.hpp"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <hb.h>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "nordend/core/log.hpp"

namespace nordend::ui {
namespace {

constexpr char32_t kReplacement = 0xFFFD;
constexpr std::string_view kEllipsis = "…";
constexpr std::size_t kMaxCachedLayouts = 8192;
constexpr float kFixedPoint = 64.0F;  // FreeType and harfbuzz 26.6 units
// Points per logical pixel, as Pango and Qt count them (96 dpi): harfbuzz picks size-dependent
// tracking (the AAT trak table) by point size.
constexpr float kPointsPerPixel = 0.75F;

// How glyphs are rasterized, from fontconfig's settings for the family (hinting, hintstyle,
// antialias, autohint), the way Pango/cairo and Qt read them.
struct RenderSettings {
    FT_Int32 load_flags{FT_LOAD_TARGET_LIGHT};
    FT_Render_Mode render_mode{FT_RENDER_MODE_LIGHT};
};

RenderSettings render_settings(const std::string& family) {
    RenderSettings settings;
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));  // NOLINT
    if (pattern == nullptr) {
        return settings;
    }
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    if (match == nullptr) {
        return settings;
    }
    FcBool antialias = FcTrue;
    FcBool hinting = FcTrue;
    FcBool autohint = FcFalse;
    int hintstyle = FC_HINT_SLIGHT;
    FcPatternGetBool(match, FC_ANTIALIAS, 0, &antialias);
    FcPatternGetBool(match, FC_HINTING, 0, &hinting);
    FcPatternGetBool(match, FC_AUTOHINT, 0, &autohint);
    FcPatternGetInteger(match, FC_HINT_STYLE, 0, &hintstyle);
    FcPatternDestroy(match);

    if (antialias == FcFalse) {
        settings.load_flags = FT_LOAD_TARGET_MONO;
        settings.render_mode = FT_RENDER_MODE_MONO;
    } else if (hinting == FcFalse || hintstyle == FC_HINT_NONE) {
        settings.load_flags = FT_LOAD_NO_HINTING;
        settings.render_mode = FT_RENDER_MODE_NORMAL;
    } else if (hintstyle == FC_HINT_SLIGHT) {
        settings.load_flags = FT_LOAD_TARGET_LIGHT;
        settings.render_mode = FT_RENDER_MODE_LIGHT;
    } else {
        settings.load_flags = FT_LOAD_TARGET_NORMAL;
        settings.render_mode = FT_RENDER_MODE_NORMAL;
    }
    if (autohint == FcTrue && hinting == FcTrue) {
        settings.load_flags |= FT_LOAD_FORCE_AUTOHINT;
    }
    return settings;
}

struct Codepoint {
    char32_t value;
    std::uint32_t offset;  // byte offset in the source text
};

// Decodes UTF-8; every byte of an invalid or truncated sequence becomes U+FFFD, the same
// policy as hb_buffer_add_utf8, so runs and clusters agree.
std::vector<Codepoint> decode_utf8(std::string_view text) {
    std::vector<Codepoint> out;
    out.reserve(text.size());
    const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
    std::size_t i = 0;
    while (i < text.size()) {
        const std::uint8_t lead = byte(i);
        std::size_t length = 0;
        char32_t value = 0;
        if (lead < 0x80U) {
            length = 1;
            value = lead;
        } else if ((lead & 0xE0U) == 0xC0U && lead >= 0xC2U) {
            length = 2;
            value = lead & 0x1FU;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
            value = lead & 0x0FU;
        } else if ((lead & 0xF8U) == 0xF0U && lead <= 0xF4U) {
            length = 4;
            value = lead & 0x07U;
        }
        bool valid = length > 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const std::uint8_t next = byte(i + k);
            valid = (next & 0xC0U) == 0x80U;
            value = (value << 6U) | (next & 0x3FU);
        }
        // Reject overlong forms, surrogates and values past U+10FFFF.
        if (valid && ((length == 3 && value < 0x800U) || (length == 4 && value < 0x10000U) ||
                      (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU)) {
            valid = false;
        }
        if (!valid) {
            out.push_back({kReplacement, static_cast<std::uint32_t>(i)});
            i += 1;
            continue;
        }
        out.push_back({value, static_cast<std::uint32_t>(i)});
        i += length;
    }
    return out;
}

// Characters that must stay in the font of the preceding character: combining marks, joiners,
// variation selectors, emoji modifiers and tags.
// Characters shown as color emoji by default (Unicode Emoji_Presentation, approximated by its
// main blocks). Text-default symbols such as U+2764 become emoji only with VS16 (U+FE0F).
bool emoji_presentation(char32_t cp) {
    return (cp >= 0x1F300U && cp <= 0x1F64FU) || (cp >= 0x1F680U && cp <= 0x1F6FFU) ||
           (cp >= 0x1F900U && cp <= 0x1FAFFU) || (cp >= 0x1F1E6U && cp <= 0x1F1FFU) ||
           cp == 0x1F004U || cp == 0x1F0CFU || cp == 0x1F18EU || (cp >= 0x1F191U && cp <= 0x1F19AU);
}

bool attaches_to_previous(char32_t cp) {
    if (cp == 0x200DU || (cp >= 0xFE00U && cp <= 0xFE0FU) || (cp >= 0x1F3FBU && cp <= 0x1F3FFU) ||
        (cp >= 0xE0020U && cp <= 0xE007FU)) {
        return true;
    }
    switch (hb_unicode_general_category(hb_unicode_funcs_get_default(), cp)) {
        case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
        case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
            return true;
        default:
            return false;
    }
}

// Area-average downscale of a premultiplied RGBA bitmap (color emoji come as large strikes).
std::vector<std::uint8_t> downscale_rgba(std::span<const std::uint8_t> src, std::int32_t src_w,
                                         std::int32_t src_h, std::int32_t dst_w,
                                         std::int32_t dst_h) {
    std::vector<std::uint8_t> dst(static_cast<std::size_t>(dst_w) *
                                  static_cast<std::size_t>(dst_h) * 4);
    const float sx = static_cast<float>(src_w) / static_cast<float>(dst_w);
    const float sy = static_cast<float>(src_h) / static_cast<float>(dst_h);
    for (std::int32_t y = 0; y < dst_h; ++y) {
        const auto y0 = static_cast<std::int32_t>(static_cast<float>(y) * sy);
        const auto y1 = std::max(y0 + 1, static_cast<std::int32_t>(static_cast<float>(y + 1) * sy));
        for (std::int32_t x = 0; x < dst_w; ++x) {
            const auto x0 = static_cast<std::int32_t>(static_cast<float>(x) * sx);
            const auto x1 =
                std::max(x0 + 1, static_cast<std::int32_t>(static_cast<float>(x + 1) * sx));
            std::array<std::uint32_t, 4> sum{};
            std::uint32_t count = 0;
            for (std::int32_t yy = y0; yy < std::min(y1, src_h); ++yy) {
                for (std::int32_t xx = x0; xx < std::min(x1, src_w); ++xx) {
                    const auto p = (static_cast<std::size_t>(yy) * static_cast<std::size_t>(src_w) +
                                    static_cast<std::size_t>(xx)) *
                                   4;
                    for (std::size_t c = 0; c < 4; ++c) {
                        sum.at(c) += src[p + c];
                    }
                    ++count;
                }
            }
            const auto d = (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_w) +
                            static_cast<std::size_t>(x)) *
                           4;
            for (std::size_t c = 0; c < 4; ++c) {
                dst[d + c] = static_cast<std::uint8_t>(count > 0 ? sum.at(c) / count : 0);
            }
        }
    }
    return dst;
}

}  // namespace

struct FontSystem::Impl {
    struct Font {
        std::string path;
        // A font in memory: path is only its name. The {} keeps GCC's
        // -Wmissing-field-initializers quiet.
        std::span<const unsigned char> data{};  // NOLINT(readability-redundant-member-init)
        int index{0};
        FcCharSet* charset{nullptr};
        FT_Face face{nullptr};
        hb_font_t* hb{nullptr};
        bool color{false};
        bool loaded{false};
        bool failed{false};
    };

    FT_Library library{nullptr};
    std::vector<Font> fonts;  // every font: the chains below and add_font_file's
    // The fontconfig fallback chain of each typeface, primary first, as indexes into fonts.
    std::array<std::vector<std::uint32_t>, 2> chains;
    std::string monospace_family;
    bool monospace_found{false};
    hb_buffer_t* buffer{hb_buffer_create()};
    std::unordered_map<std::string, TextLayout> cache;
    RenderSettings render;
    float ptem{0.0F};  // point size of the text being shaped; 0: harfbuzz's default

    Impl(const std::string& family, std::string monospace)
        : monospace_family{std::move(monospace)} {
        if (FT_Init_FreeType(&library) != 0) {
            throw std::runtime_error{"FreeType initialization failed"};
        }
        std::vector<std::uint32_t>& primary = chains[0];
        primary = discover(family);
        if (primary.empty() || !load(primary.front())) {
            throw std::runtime_error{std::format("no usable font for '{}'", family)};
        }
        log::info("font: {} ({} fallbacks)", fonts[primary.front()].path, primary.size() - 1);
        render = render_settings(family);
    }

    // The chain of a typeface; the monospace one is looked up the first time, and is the
    // proportional one when no monospace font loads.
    const std::vector<std::uint32_t>& chain(Typeface typeface) {
        if (typeface == Typeface::proportional) {
            return chains[0];
        }
        if (!monospace_found) {
            monospace_found = true;
            chains[1] = discover(monospace_family);
            if (chains[1].empty() || !load(chains[1].front())) {
                log::warn("no usable monospace font for '{}'", monospace_family);
                chains[1] = chains[0];
            } else {
                log::info("monospace font: {} ({} fallbacks)", fonts[chains[1].front()].path,
                          chains[1].size() - 1);
            }
        }
        return chains[1];
    }

    ~Impl() {
        for (Font& font : fonts) {
            if (font.hb != nullptr) {
                hb_font_destroy(font.hb);
            }
            if (font.face != nullptr) {
                FT_Done_Face(font.face);
            }
            FcCharSetDestroy(font.charset);
        }
        hb_buffer_destroy(buffer);
        FT_Done_FreeType(library);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    std::vector<std::uint32_t> discover(const std::string& family) {
        std::vector<std::uint32_t> chain;
        FcPattern* pattern =
            FcNameParse(reinterpret_cast<const FcChar8*>(family.c_str()));  // NOLINT
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result = FcResultNoMatch;
        // trim: keep only fonts that add coverage, in preference order.
        FcFontSet* set = FcFontSort(nullptr, pattern, FcTrue, nullptr, &result);
        FcPatternDestroy(pattern);
        if (set == nullptr) {
            return chain;
        }
        for (const FcPattern* candidate :
             std::span{set->fonts, static_cast<std::size_t>(set->nfont)}) {
            FcChar8* file = nullptr;
            int index = 0;
            FcCharSet* charset = nullptr;
            if (FcPatternGetString(candidate, FC_FILE, 0, &file) != FcResultMatch ||
                FcPatternGetCharSet(candidate, FC_CHARSET, 0, &charset) != FcResultMatch) {
                continue;
            }
            FcPatternGetInteger(candidate, FC_INDEX, 0, &index);
            const std::string path = reinterpret_cast<const char*>(file);  // NOLINT
            // Already known from the other chain: the same font, loaded once.
            const auto known = std::ranges::find_if(
                fonts, [&](const Font& font) { return font.path == path && font.index == index; });
            if (known != fonts.end()) {
                chain.push_back(static_cast<std::uint32_t>(known - fonts.begin()));
                continue;
            }
            FcBool color = FcFalse;
            FcPatternGetBool(candidate, FC_COLOR, 0, &color);
            fonts.push_back({.path = path,
                             .index = index,
                             .charset = FcCharSetCopy(charset),
                             .color = color == FcTrue});
            chain.push_back(static_cast<std::uint32_t>(fonts.size() - 1));
        }
        FcFontSetDestroy(set);
        return chain;
    }

    bool load(std::size_t id) {
        Font& font = fonts.at(id);
        if (font.loaded || font.failed) {
            return font.loaded;
        }
        const bool in_memory = !font.data.empty();
        const FT_Error error =
            in_memory
                ? FT_New_Memory_Face(library, font.data.data(),
                                     static_cast<FT_Long>(font.data.size()), font.index, &font.face)
                : FT_New_Face(library, font.path.c_str(), font.index, &font.face);
        if (error != 0) {
            log::warn("cannot load font {}", font.path);
            font.failed = true;
            return false;
        }
        font.color = FT_HAS_COLOR(font.face);
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): harfbuzz takes char bytes
        hb_blob_t* blob = in_memory
                              ? hb_blob_create(reinterpret_cast<const char*>(font.data.data()),
                                               static_cast<unsigned>(font.data.size()),
                                               HB_MEMORY_MODE_READONLY, nullptr, nullptr)
                              : hb_blob_create_from_file(font.path.c_str());
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        hb_face_t* face = hb_face_create(blob, static_cast<unsigned>(font.index));
        font.hb = hb_font_create(face);
        hb_face_destroy(face);
        hb_blob_destroy(blob);
        font.loaded = true;
        log::debug("loaded font {} #{}", font.path, font.index);
        return true;
    }

    // First font of the chain that has cp. Emoji prefer a color font: fontconfig often ranks
    // monochrome symbol fonts that also cover them higher.
    std::uint32_t font_for(char32_t cp, bool prefer_color,
                           const std::vector<std::uint32_t>& fallbacks) {
        if (prefer_color) {
            for (const std::uint32_t id : fallbacks) {
                if (fonts[id].color && FcCharSetHasChar(fonts[id].charset, cp) == FcTrue &&
                    load(id)) {
                    return id;
                }
            }
        }
        for (const std::uint32_t id : fallbacks) {
            if (FcCharSetHasChar(fonts[id].charset, cp) == FcTrue && load(id)) {
                return id;
            }
        }
        // The primary font: the missing glyph (.notdef box) is the honest result.
        return fallbacks.front();
    }

    TextLayout shape(std::string_view text, float pixel_size, Typeface typeface) {
        TextLayout layout;
        layout.pixel_size = pixel_size;
        const auto scale = static_cast<int>(std::lround(pixel_size * kFixedPoint));
        const std::vector<std::uint32_t>& fallbacks = chain(typeface);
        hb_font_t* const primary = fonts[fallbacks.front()].hb;

        hb_font_set_scale(primary, scale, scale);
        hb_font_set_ptem(primary, ptem);
        hb_font_extents_t extents{};
        hb_font_get_h_extents(primary, &extents);
        layout.ascent = static_cast<float>(extents.ascender) / kFixedPoint;
        layout.descent = static_cast<float>(-extents.descender) / kFixedPoint;

        const std::vector<Codepoint> codepoints = decode_utf8(text);
        float pen = 0.0F;
        std::size_t run_start = 0;
        std::uint32_t run_font = 0;
        for (std::size_t i = 0; i <= codepoints.size(); ++i) {
            std::uint32_t font = run_font;
            if (i < codepoints.size() && (i == 0 || !attaches_to_previous(codepoints[i].value))) {
                const char32_t cp = codepoints[i].value;
                const bool vs16 = i + 1 < codepoints.size() && codepoints[i + 1].value == 0xFE0FU;
                font = font_for(cp, vs16 || emoji_presentation(cp), fallbacks);
            }
            if (i == 0) {
                run_font = font;
                continue;
            }
            if (i < codepoints.size() && font == run_font) {
                continue;
            }
            const std::uint32_t begin = codepoints[run_start].offset;
            const auto end = i < codepoints.size() ? codepoints[i].offset
                                                   : static_cast<std::uint32_t>(text.size());
            pen = shape_run(text, begin, end, run_font, scale, pen, layout.glyphs);
            run_start = i;
            run_font = font;
        }
        layout.width = pen;
        return layout;
    }

    float shape_run(std::string_view text, std::uint32_t begin, std::uint32_t end,
                    std::uint32_t font_id, int scale, float pen, std::vector<ShapedGlyph>& out) {
        hb_font_t* font = fonts[font_id].hb;
        hb_font_set_scale(font, scale, scale);
        hb_font_set_ptem(font, ptem);
        hb_buffer_reset(buffer);
        // The whole text gives the shaper context; clusters are byte offsets into it.
        hb_buffer_add_utf8(buffer, text.data(), static_cast<int>(text.size()),
                           static_cast<unsigned>(begin), static_cast<int>(end - begin));
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(font, buffer, nullptr, 0);

        unsigned count = 0;
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
        const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
        const std::span info_span{infos, count};
        const std::span position_span{positions, count};
        for (std::size_t g = 0; g < count; ++g) {
            const float advance = static_cast<float>(position_span[g].x_advance) / kFixedPoint;
            out.push_back({
                .font = font_id,
                .glyph = info_span[g].codepoint,
                .cluster = info_span[g].cluster,
                .x = pen + (static_cast<float>(position_span[g].x_offset) / kFixedPoint),
                .y = -static_cast<float>(position_span[g].y_offset) / kFixedPoint,
                .advance = advance,
            });
            pen += advance;
        }
        return pen;
    }

    TextLayout truncate(std::string_view text, const TextLayout& full, float max_px,
                        Ellipsis ellipsis, Typeface typeface) {
        // Width of each cluster, in byte order (left-to-right text).
        std::vector<std::pair<std::uint32_t, float>> clusters;
        for (const ShapedGlyph& glyph : full.glyphs) {
            if (!clusters.empty() && clusters.back().first == glyph.cluster) {
                clusters.back().second += glyph.advance;
            } else {
                clusters.emplace_back(glyph.cluster, glyph.advance);
            }
        }
        std::ranges::sort(clusters, {}, &std::pair<std::uint32_t, float>::first);
        const auto start_of = [&](std::size_t k) {
            return k < clusters.size() ? clusters[k].first
                                       : static_cast<std::uint32_t>(text.size());
        };

        const float ellipsis_width = shape(kEllipsis, full.pixel_size, typeface).width;
        const float budget = std::max(0.0F, max_px - ellipsis_width);

        std::string result;
        if (ellipsis == Ellipsis::end) {
            std::size_t head = 0;
            float used = 0.0F;
            while (head < clusters.size() && used + clusters[head].second <= budget) {
                used += clusters[head++].second;
            }
            result = std::string{text.substr(0, start_of(head))} + std::string{kEllipsis};
        } else {
            std::size_t head = 0;
            float used = 0.0F;
            while (head < clusters.size() && used + clusters[head].second <= budget / 2.0F) {
                used += clusters[head++].second;
            }
            std::size_t tail = clusters.size();
            while (tail > head && used + clusters[tail - 1].second <= budget) {
                used += clusters[--tail].second;
            }
            result = std::string{text.substr(0, start_of(head))} + std::string{kEllipsis} +
                     std::string{text.substr(start_of(tail))};
        }
        TextLayout layout = shape(result, full.pixel_size, typeface);
        layout.truncated = true;
        return layout;
    }
};

FontSystem::FontSystem(const std::string& family, std::string monospace)
    : impl_{std::make_unique<Impl>(family, std::move(monospace))} {}

FontSystem::~FontSystem() = default;

const TextLayout& FontSystem::layout(std::string_view utf8, float size, float scale,
                                     float max_width, Ellipsis ellipsis, Typeface typeface) {
    const float pixel_size = size * scale;
    const float max_px = max_width * scale;
    std::string key{utf8};
    key += std::format("\x1f{}\x1f{}\x1f{}\x1f{}\x1f{}", pixel_size, size, max_px,
                       static_cast<int>(ellipsis), static_cast<int>(typeface));

    auto& cache = impl_->cache;
    if (auto it = cache.find(key); it != cache.end()) {
        return it->second;
    }
    if (cache.size() >= kMaxCachedLayouts) {
        cache.clear();
    }
    impl_->ptem = size * kPointsPerPixel;
    TextLayout layout = impl_->shape(utf8, pixel_size, typeface);
    if (max_px > 0.0F && layout.width > max_px) {
        layout = impl_->truncate(utf8, layout, max_px, ellipsis, typeface);
    }
    return cache.emplace(std::move(key), std::move(layout)).first->second;
}

std::optional<std::uint32_t> FontSystem::add_font_file(const std::string& path) {
    return add_font_data({}, path);
}

std::optional<std::uint32_t> FontSystem::add_font_data(std::span<const unsigned char> data,
                                                       const std::string& name) {
    auto& fonts = impl_->fonts;
    // An empty charset keeps the font out of text fallback.
    fonts.push_back({.path = name, .data = data, .index = 0, .charset = FcCharSetCreate()});
    const std::size_t id = fonts.size() - 1;
    if (!impl_->load(id)) {
        FcCharSetDestroy(fonts.back().charset);
        fonts.pop_back();
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(id);
}

std::uint32_t FontSystem::glyph_by_name(std::uint32_t font, std::string_view name) {
    if (!impl_->load(font)) {
        return 0;
    }
    std::string zero_terminated{name};
    return FT_Get_Name_Index(impl_->fonts.at(font).face, zero_terminated.data());
}

TextLayout FontSystem::glyph_layout(std::uint32_t font, std::uint32_t glyph, float size,
                                    float scale) {
    TextLayout layout;
    const float pixel_size = size * scale;
    layout.pixel_size = pixel_size;
    layout.width = pixel_size;
    if (glyph == 0 || !impl_->load(font)) {
        return layout;
    }
    FT_Face face = impl_->fonts.at(font).face;
    if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE) != 0 || face->units_per_EM == 0) {
        return layout;
    }
    // Center the glyph's bounding box in the box: its center goes to (size / 2, size / 2).
    const FT_Glyph_Metrics& metrics = face->glyph->metrics;
    const float units = pixel_size / static_cast<float>(face->units_per_EM);
    const float center_x =
        (static_cast<float>(metrics.horiBearingX) + (static_cast<float>(metrics.width) / 2.0F)) *
        units;
    const float center_y =  // above the baseline
        (static_cast<float>(metrics.horiBearingY) - (static_cast<float>(metrics.height) / 2.0F)) *
        units;
    layout.ascent = (pixel_size / 2.0F) + center_y;
    layout.descent = pixel_size - layout.ascent;
    layout.glyphs.push_back({.font = font,
                             .glyph = glyph,
                             .cluster = 0,
                             .x = (pixel_size / 2.0F) - center_x,
                             .y = 0.0F,
                             .advance = pixel_size});
    return layout;
}

GlyphBitmap FontSystem::rasterize(std::uint32_t font_id, std::uint32_t glyph, float pixel_size,
                                  float x_offset) {
    GlyphBitmap bitmap;
    Impl::Font& font = impl_->fonts.at(font_id);
    if (!impl_->load(font_id)) {
        return bitmap;
    }
    FT_Face face = font.face;

    // Color emoji fonts (CBDT/sbix) only have fixed strikes: pick the smallest one at least as
    // big as wanted, then scale the bitmap down.
    float strike_scale = 1.0F;
    if (FT_IS_SCALABLE(face)) {
        FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(std::lround(pixel_size * kFixedPoint)),
                         72, 72);
    } else if (face->num_fixed_sizes > 0) {
        const std::span sizes{face->available_sizes,
                              static_cast<std::size_t>(face->num_fixed_sizes)};
        std::size_t best = 0;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            const bool fits = static_cast<float>(sizes[i].height) >= pixel_size;
            const bool best_fits = static_cast<float>(sizes[best].height) >= pixel_size;
            if ((fits && (!best_fits || sizes[i].height < sizes[best].height)) ||
                (!fits && !best_fits && sizes[i].height > sizes[best].height)) {
                best = i;
            }
        }
        FT_Select_Size(face, static_cast<FT_Int>(best));
        strike_scale = pixel_size / static_cast<float>(sizes[best].height);
    }

    FT_Int32 flags = impl_->render.load_flags;
    FT_Render_Mode mode = impl_->render.render_mode;
    if (font.color) {
        flags = FT_LOAD_TARGET_LIGHT | FT_LOAD_COLOR;
        mode = FT_RENDER_MODE_LIGHT;
    }
    // Subpixel positioning: the outline moves by the fraction of a pixel the pen stands at.
    FT_Vector delta{static_cast<FT_Pos>(std::lround(x_offset * kFixedPoint)), 0};
    FT_Set_Transform(face, nullptr, FT_IS_SCALABLE(face) && !font.color ? &delta : nullptr);
    const bool loaded = FT_Load_Glyph(face, glyph, flags) == 0;
    FT_Set_Transform(face, nullptr, nullptr);
    if (!loaded || FT_Render_Glyph(face->glyph, mode) != 0) {
        return bitmap;
    }
    const FT_Bitmap& ft = face->glyph->bitmap;
    const auto width = static_cast<std::int32_t>(ft.width);
    const auto height = static_cast<std::int32_t>(ft.rows);
    bitmap.left = face->glyph->bitmap_left;
    bitmap.top = face->glyph->bitmap_top;
    if (width == 0 || height == 0) {
        return bitmap;  // space and other blank glyphs
    }

    const auto row = [&](std::int32_t y) {
        return ft.buffer + (static_cast<std::ptrdiff_t>(y) * ft.pitch);  // NOLINT
    };
    switch (ft.pixel_mode) {
        case FT_PIXEL_MODE_GRAY:
            bitmap.pixels.resize(static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height));
            for (std::int32_t y = 0; y < height; ++y) {
                std::copy_n(row(y), width,
                            bitmap.pixels.begin() + static_cast<std::ptrdiff_t>(y) * width);
            }
            break;
        case FT_PIXEL_MODE_MONO:
            bitmap.pixels.resize(static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height));
            for (std::int32_t y = 0; y < height; ++y) {
                const std::span bits{row(y), static_cast<std::size_t>((width + 7) / 8)};
                for (std::int32_t x = 0; x < width; ++x) {
                    const auto set =
                        (static_cast<unsigned>(bits[static_cast<std::size_t>(x / 8)]) >>
                         (7U - static_cast<unsigned>(x % 8))) &
                        1U;
                    bitmap.pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                                  static_cast<std::size_t>(x)] = set != 0 ? 0xFF : 0x00;
                }
            }
            break;
        case FT_PIXEL_MODE_BGRA: {
            bitmap.color = true;
            std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) *
                                           static_cast<std::size_t>(height) * 4);
            for (std::int32_t y = 0; y < height; ++y) {
                const std::span src{row(y), static_cast<std::size_t>(width) * 4};
                for (std::int32_t x = 0; x < width; ++x) {
                    const auto s = static_cast<std::size_t>(x) * 4;
                    const auto d =
                        ((static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
                         static_cast<std::size_t>(x)) *
                        4;
                    rgba[d] = src[s + 2];  // FreeType BGRA is premultiplied already
                    rgba[d + 1] = src[s + 1];
                    rgba[d + 2] = src[s];
                    rgba[d + 3] = src[s + 3];
                }
            }
            if (strike_scale < 1.0F) {
                const auto scaled_w = std::max(1, static_cast<std::int32_t>(std::lround(
                                                      static_cast<float>(width) * strike_scale)));
                const auto scaled_h = std::max(1, static_cast<std::int32_t>(std::lround(
                                                      static_cast<float>(height) * strike_scale)));
                bitmap.pixels = downscale_rgba(rgba, width, height, scaled_w, scaled_h);
                bitmap.width = scaled_w;
                bitmap.height = scaled_h;
                bitmap.left = static_cast<std::int32_t>(
                    std::lround(static_cast<float>(bitmap.left) * strike_scale));
                bitmap.top = static_cast<std::int32_t>(
                    std::lround(static_cast<float>(bitmap.top) * strike_scale));
                return bitmap;
            }
            bitmap.pixels = std::move(rgba);
            break;
        }
        default:
            log::debug("glyph {} of {}: unsupported pixel mode {}", glyph, font.path,
                       static_cast<int>(ft.pixel_mode));
            return bitmap;
    }
    bitmap.width = width;
    bitmap.height = height;
    return bitmap;
}

}  // namespace nordend::ui
