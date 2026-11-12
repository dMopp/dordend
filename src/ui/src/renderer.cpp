#include "nordend/ui/renderer.hpp"

#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "nordend/core/log.hpp"

namespace nordend::ui {
namespace {

// Boxes: one instanced quad per primitive. The fragment shader evaluates the signed distance to
// a rounded box (per-corner radii) in physical pixels: coverage gives 1-pixel antialiasing, an
// inner offset of the same distance gives the border, and a Gaussian falloff of the distance
// gives the shadow (an approximation of a blurred rounded box, exact on straight edges).
constexpr const char* kBoxVertexShader = R"glsl(#version 300 es
layout(location = 0) in vec2 a_corner;        // unit quad, 0..1
layout(location = 1) in vec4 a_rect;          // physical x, y, width, height
layout(location = 2) in vec4 a_radii;         // top-left, top-right, bottom-right, bottom-left
layout(location = 3) in vec4 a_fill;          // premultiplied
layout(location = 4) in vec4 a_border_color;  // premultiplied
layout(location = 5) in vec2 a_params;        // border width, shadow sigma (0 = box)

uniform vec2 u_viewport;

out vec2 v_pos;
flat out vec4 v_rect;
flat out vec4 v_radii;
flat out vec4 v_fill;
flat out vec4 v_border_color;
flat out vec2 v_params;

void main() {
    float pad = a_params.y > 0.0 ? 3.0 * a_params.y : 1.0;
    vec2 pos = a_rect.xy - pad + a_corner * (a_rect.zw + 2.0 * pad);
    v_pos = pos;
    v_rect = a_rect;
    v_radii = a_radii;
    v_fill = a_fill;
    v_border_color = a_border_color;
    v_params = a_params;
    vec2 ndc = pos / u_viewport * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)glsl";

constexpr const char* kBoxFragmentShader = R"glsl(#version 300 es
precision highp float;

in vec2 v_pos;
flat in vec4 v_rect;
flat in vec4 v_radii;
flat in vec4 v_fill;
flat in vec4 v_border_color;
flat in vec2 v_params;

out vec4 o_color;

float rounded_box_distance(vec2 p, vec2 half_size, vec4 radii) {
    float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
    r = min(r, min(half_size.x, half_size.y));
    vec2 q = abs(p) - half_size + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Abramowitz-Stegun style approximation of erf, max error ~5e-4.
float erf_approx(float x) {
    float s = sign(x);
    float a = abs(x);
    float t = 1.0 + (0.278393 + (0.230389 + 0.078108 * (a * a)) * a) * a;
    t *= t;
    return s - s / (t * t);
}

void main() {
    vec2 half_size = v_rect.zw * 0.5;
    vec2 p = v_pos - (v_rect.xy + half_size);
    float d = rounded_box_distance(p, half_size, v_radii);

    float sigma = v_params.y;
    if (sigma > 0.0) {
        float alpha = 0.5 - 0.5 * erf_approx(d / (sigma * 1.41421356));
        o_color = v_fill * alpha;
        return;
    }

    float coverage = clamp(0.5 - d, 0.0, 1.0);
    float inner = clamp(0.5 - (d + v_params.x), 0.0, 1.0);
    o_color = v_fill * inner + v_border_color * (coverage - inner);
}
)glsl";

// Glyphs: one textured quad per glyph, mapped 1:1 onto physical pixels. Coverage glyphs come
// from the R8 atlas and take the text color; color glyphs (emoji) come from the RGBA atlas and
// only take its alpha.
constexpr const char* kGlyphVertexShader = R"glsl(#version 300 es
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_rect;   // physical x, y, width, height
layout(location = 2) in vec4 a_uv;     // u0, v0, u1, v1
layout(location = 3) in vec4 a_color;  // premultiplied
layout(location = 4) in float a_kind;  // 0 = coverage, 1 = color

uniform vec2 u_viewport;

out vec2 v_uv;
flat out vec4 v_color;
flat out float v_kind;

void main() {
    vec2 pos = a_rect.xy + a_corner * a_rect.zw;
    v_uv = mix(a_uv.xy, a_uv.zw, a_corner);
    v_color = a_color;
    v_kind = a_kind;
    vec2 ndc = pos / u_viewport * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)glsl";

constexpr const char* kGlyphFragmentShader = R"glsl(#version 300 es
precision highp float;

in vec2 v_uv;
flat in vec4 v_color;
flat in float v_kind;

uniform sampler2D u_coverage;
uniform sampler2D u_color;

out vec4 o_color;

void main() {
    if (v_kind < 0.5) {
        o_color = v_color * texture(u_coverage, v_uv).r;
    } else {
        o_color = texture(u_color, v_uv) * v_color.a;
    }
}
)glsl";

// Images: one textured quad, cut to a rounded box with the same distance function as the boxes.
constexpr const char* kImageVertexShader = R"glsl(#version 300 es
layout(location = 0) in vec2 a_corner;
layout(location = 1) in vec4 a_rect;   // physical x, y, width, height
layout(location = 2) in vec4 a_radii;  // top-left, top-right, bottom-right, bottom-left
layout(location = 3) in float a_opacity;

uniform vec2 u_viewport;

out vec2 v_pos;
out vec2 v_uv;
flat out vec4 v_rect;
flat out vec4 v_radii;
flat out float v_opacity;

void main() {
    vec2 pos = a_rect.xy + a_corner * a_rect.zw;
    v_pos = pos;
    v_uv = a_corner;
    v_rect = a_rect;
    v_radii = a_radii;
    v_opacity = a_opacity;
    vec2 ndc = pos / u_viewport * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)glsl";

constexpr const char* kImageFragmentShader = R"glsl(#version 300 es
precision highp float;

in vec2 v_pos;
in vec2 v_uv;
flat in vec4 v_rect;
flat in vec4 v_radii;
flat in float v_opacity;

uniform sampler2D u_image;  // premultiplied

out vec4 o_color;

float rounded_box_distance(vec2 p, vec2 half_size, vec4 radii) {
    float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
    r = min(r, min(half_size.x, half_size.y));
    vec2 q = abs(p) - half_size + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    vec2 half_size = v_rect.zw * 0.5;
    float d = rounded_box_distance(v_pos - (v_rect.xy + half_size), half_size, v_radii);
    float coverage = clamp(0.5 - d, 0.0, 1.0);
    o_color = texture(u_image, v_uv) * (coverage * v_opacity);
}
)glsl";

constexpr std::size_t kMaxInstancesPerDraw = 4096;
constexpr std::int32_t kAtlasSize = 2048;
constexpr std::int32_t kAtlasPadding = 1;  // keeps linear filtering from bleeding neighbours

GLuint compile_shader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        std::array<char, 1024> info{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(info.size()), nullptr, info.data());
        glDeleteShader(shader);
        throw std::runtime_error{std::format("shader compile failed: {}", info.data())};
    }
    return shader;
}

GLuint link_program(const char* vertex_source, const char* fragment_source) {
    const GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertex_source);
    GLuint fragment = 0;
    try {
        fragment = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    } catch (...) {
        glDeleteShader(vertex);
        throw;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);  // flagged; freed with the program
    glDeleteShader(fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        std::array<char, 1024> info{};
        glGetProgramInfoLog(program, static_cast<GLsizei>(info.size()), nullptr, info.data());
        glDeleteProgram(program);
        throw std::runtime_error{std::format("shader link failed: {}", info.data())};
    }
    return program;
}

// Logical -> physical, rounded to the nearest pixel edge.
float snap(float logical, float scale) { return std::round(logical * scale); }

struct Attribute {
    GLuint location;
    GLint components;
    std::size_t offset;
};

// A program drawing instanced unit quads, with the per-instance attributes it reads.
class Pipeline {
public:
    Pipeline(const char* vertex_source, const char* fragment_source,
             std::span<const Attribute> attributes, std::size_t stride)
        : program_{link_program(vertex_source, fragment_source)},
          viewport_uniform_{glGetUniformLocation(program_, "u_viewport")} {
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);

        static constexpr std::array<float, 8> kQuad{0, 0, 1, 0, 0, 1, 1, 1};
        glGenBuffers(1, &quad_vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, quad_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

        glGenBuffers(1, &instance_vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, instance_vbo_);
        for (const Attribute& attribute : attributes) {
            glEnableVertexAttribArray(attribute.location);
            // GL takes buffer offsets as pointers.
            glVertexAttribPointer(attribute.location, attribute.components, GL_FLOAT, GL_FALSE,
                                  static_cast<GLsizei>(stride),
                                  reinterpret_cast<const void*>(attribute.offset));  // NOLINT
            glVertexAttribDivisor(attribute.location, 1);
        }
        glBindVertexArray(0);
    }

    ~Pipeline() {
        glDeleteBuffers(1, &instance_vbo_);
        glDeleteBuffers(1, &quad_vbo_);
        glDeleteVertexArrays(1, &vao_);
        glDeleteProgram(program_);
    }

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    Pipeline(Pipeline&&) = delete;
    Pipeline& operator=(Pipeline&&) = delete;

    [[nodiscard]] GLuint program() const noexcept { return program_; }

    void draw(const void* instances, std::size_t count, std::size_t stride,
              std::int32_t viewport_width, std::int32_t viewport_height) const {
        glUseProgram(program_);
        glUniform2f(viewport_uniform_, static_cast<float>(viewport_width),
                    static_cast<float>(viewport_height));
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, instance_vbo_);
        // Orphan the previous storage so the driver never waits for the last draw to finish.
        const auto bytes = static_cast<GLsizeiptr>(count * stride);
        glBufferData(GL_ARRAY_BUFFER, bytes, nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, instances);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(count));
        glBindVertexArray(0);
    }

private:
    GLuint program_;
    GLint viewport_uniform_;
    GLuint vao_{0};
    GLuint quad_vbo_{0};
    GLuint instance_vbo_{0};
};

// One texture packed with glyph bitmaps in shelves (rows of the height of their tallest glyph).
class Atlas {
public:
    Atlas(GLint internal_format, GLenum format) : format_{format} {
        glGenTextures(1, &texture_);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, kAtlasSize, kAtlasSize, 0, format,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    ~Atlas() { glDeleteTextures(1, &texture_); }

    Atlas(const Atlas&) = delete;
    Atlas& operator=(const Atlas&) = delete;
    Atlas(Atlas&&) = delete;
    Atlas& operator=(Atlas&&) = delete;

    [[nodiscard]] GLuint texture() const noexcept { return texture_; }

    // Uploads the bitmap and returns its top-left texel, or false when the atlas is full.
    bool insert(std::int32_t width, std::int32_t height, const std::uint8_t* pixels,
                std::int32_t& out_x, std::int32_t& out_y) {
        if (width + kAtlasPadding > kAtlasSize || height + kAtlasPadding > kAtlasSize) {
            return false;
        }
        if (cursor_x_ + width + kAtlasPadding > kAtlasSize) {
            cursor_x_ = 0;
            cursor_y_ += shelf_height_ + kAtlasPadding;
            shelf_height_ = 0;
        }
        if (cursor_y_ + height + kAtlasPadding > kAtlasSize) {
            return false;
        }
        out_x = cursor_x_;
        out_y = cursor_y_;
        glBindTexture(GL_TEXTURE_2D, texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, out_x, out_y, width, height, format_, GL_UNSIGNED_BYTE,
                        pixels);
        cursor_x_ += width + kAtlasPadding;
        shelf_height_ = std::max(shelf_height_, height);
        return true;
    }

    void clear() noexcept {
        cursor_x_ = 0;
        cursor_y_ = 0;
        shelf_height_ = 0;
    }

private:
    GLuint texture_{0};
    GLenum format_;
    std::int32_t cursor_x_{0};
    std::int32_t cursor_y_{0};
    std::int32_t shelf_height_{0};
};

struct GlyphKey {
    std::uint32_t font;
    std::uint32_t glyph;
    std::uint32_t size_64;  // pixel size in 1/64 px
    std::uint32_t subpixel;  // horizontal position in 1/kSubpixelSteps px

    bool operator==(const GlyphKey&) const = default;
};

struct GlyphKeyHash {
    std::size_t operator()(const GlyphKey& key) const noexcept {
        const auto a = (static_cast<std::uint64_t>(key.font) << 32U) | key.glyph;
        return std::hash<std::uint64_t>{}((a * 0x9E3779B97F4A7C15ULL) ^
                                          (static_cast<std::uint64_t>(key.size_64) << 2U) ^
                                          key.subpixel);
    }
};

}  // namespace

// Where a rasterized glyph lives in its atlas; width 0 for blank glyphs.
struct Renderer::GlyphEntry {
    std::array<float, 4> uv{};
    std::int32_t width{0};
    std::int32_t height{0};
    std::int32_t left{0};
    std::int32_t top{0};
    bool color{false};
};

// Mirrors the per-instance attributes of the box shader.
struct Renderer::BoxInstance {
    std::array<float, 4> rect;
    std::array<float, 4> radii;
    std::array<float, 4> fill;
    std::array<float, 4> border_color;
    std::array<float, 2> params;
};

// Mirrors the per-instance attributes of the image shader.
struct ImageInstance {
    std::array<float, 4> rect;
    std::array<float, 4> radii;
    float opacity;
};

// Mirrors the per-instance attributes of the glyph shader.
struct Renderer::GlyphInstance {
    std::array<float, 4> rect;
    std::array<float, 4> uv;
    std::array<float, 4> color;
    float kind;
};

struct Renderer::Gl {
    static constexpr std::array<Attribute, 5> kBoxAttributes{{
        {1, 4, offsetof(BoxInstance, rect)},
        {2, 4, offsetof(BoxInstance, radii)},
        {3, 4, offsetof(BoxInstance, fill)},
        {4, 4, offsetof(BoxInstance, border_color)},
        {5, 2, offsetof(BoxInstance, params)},
    }};
    static constexpr std::array<Attribute, 4> kGlyphAttributes{{
        {1, 4, offsetof(GlyphInstance, rect)},
        {2, 4, offsetof(GlyphInstance, uv)},
        {3, 4, offsetof(GlyphInstance, color)},
        {4, 1, offsetof(GlyphInstance, kind)},
    }};

    static constexpr std::array<Attribute, 3> kImageAttributes{{
        {1, 4, offsetof(ImageInstance, rect)},
        {2, 4, offsetof(ImageInstance, radii)},
        {3, 1, offsetof(ImageInstance, opacity)},
    }};

    Pipeline boxes{kBoxVertexShader, kBoxFragmentShader, kBoxAttributes, sizeof(BoxInstance)};
    Pipeline images{kImageVertexShader, kImageFragmentShader, kImageAttributes,
                    sizeof(ImageInstance)};
    Pipeline glyphs{kGlyphVertexShader, kGlyphFragmentShader, kGlyphAttributes,
                    sizeof(GlyphInstance)};
    Atlas coverage_atlas{GL_R8, GL_RED};
    Atlas color_atlas{GL_RGBA8, GL_RGBA};
    std::unordered_map<GlyphKey, GlyphEntry, GlyphKeyHash> entries;

    Gl() {
        glUseProgram(glyphs.program());
        glUniform1i(glGetUniformLocation(glyphs.program(), "u_coverage"), 0);
        glUniform1i(glGetUniformLocation(glyphs.program(), "u_color"), 1);
        glUseProgram(images.program());
        glUniform1i(glGetUniformLocation(images.program(), "u_image"), 0);
    }
};

Renderer::Renderer(FontSystem& fonts) : gl_{std::make_unique<Gl>()}, fonts_{fonts} {
    boxes_.reserve(kMaxInstancesPerDraw);
    glyphs_.reserve(kMaxInstancesPerDraw);
}

Renderer::~Renderer() = default;

void Renderer::begin_frame(std::int32_t buffer_width, std::int32_t buffer_height, float scale,
                           Color clear, float background_opacity) {
    buffer_width_ = buffer_width;
    buffer_height_ = buffer_height;
    scale_ = scale;
    background_opacity_ = std::clamp(background_opacity, 0.0F, 1.0F);
    boxes_.clear();
    glyphs_.clear();
    clips_.clear();

    glViewport(0, 0, buffer_width_, buffer_height_);
    glDisable(GL_SCISSOR_TEST);
    clear.a *= background_opacity_;
    clear = clear.premultiplied();
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied alpha
}

void Renderer::fill_background(const Rect& rect, Color color) {
    color.a *= background_opacity_;
    color = color.premultiplied();
    // A scissored clear replaces the panel, avoiding accumulated alpha from the frame clear.
    // push_clip flushes previous draws; pop_clip restores the caller's clipping rectangle.
    push_clip(rect);
    glClearColor(color.r, color.g, color.b, color.a);
    glClear(GL_COLOR_BUFFER_BIT);
    pop_clip();
}

void Renderer::end_frame() {
    flush();
    if (!clips_.empty()) {
        log::warn("renderer: {} clip(s) still pushed at end of frame", clips_.size());
    }
}

void Renderer::draw_box(const Rect& rect, const BoxStyle& style) {
    if (rect.empty()) {
        return;
    }
    const float x0 = snap(rect.x, scale_);
    const float y0 = snap(rect.y, scale_);
    const float x1 = snap(rect.right(), scale_);
    const float y1 = snap(rect.bottom(), scale_);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    // Borders are whole physical pixels, at least one, so a 1 px border is a crisp hairline.
    const float border =
        style.border_width > 0.0F ? std::max(1.0F, snap(style.border_width, scale_)) : 0.0F;
    const Color fill = style.fill.premultiplied();
    const Color border_color = style.border.premultiplied();
    push_box({
        .rect = {x0, y0, x1 - x0, y1 - y0},
        .radii = {style.radius.top_left * scale_, style.radius.top_right * scale_,
                  style.radius.bottom_right * scale_, style.radius.bottom_left * scale_},
        .fill = {fill.r, fill.g, fill.b, fill.a},
        .border_color = {border_color.r, border_color.g, border_color.b, border_color.a},
        .params = {border, 0.0F},
    });
}

void Renderer::draw_shadow(const Rect& rect, const Corners& radius, float blur, Color color) {
    if (rect.empty() || blur <= 0.0F) {
        return;
    }
    const Color premultiplied = color.premultiplied();
    push_box({
        .rect = {rect.x * scale_, rect.y * scale_, rect.width * scale_, rect.height * scale_},
        .radii = {radius.top_left * scale_, radius.top_right * scale_, radius.bottom_right * scale_,
                  radius.bottom_left * scale_},
        .fill = {premultiplied.r, premultiplied.g, premultiplied.b, premultiplied.a},
        .border_color = {0.0F, 0.0F, 0.0F, 0.0F},
        .params = {0.0F, blur * scale_},
    });
}

void Renderer::draw_image(const Texture& texture, const Rect& rect, const Corners& radius,
                          float opacity) {
    if (rect.empty() || opacity <= 0.0F) {
        return;
    }
    // Snapped like boxes, so that an image and the box behind it share their edges.
    const float x0 = snap(rect.x, scale_);
    const float y0 = snap(rect.y, scale_);
    const float x1 = snap(rect.right(), scale_);
    const float y1 = snap(rect.bottom(), scale_);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    flush();  // paint order: what is pending goes first
    const ImageInstance instance{
        .rect = {x0, y0, x1 - x0, y1 - y0},
        .radii = {radius.top_left * scale_, radius.top_right * scale_, radius.bottom_right * scale_,
                  radius.bottom_left * scale_},
        .opacity = opacity,
    };
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture.id());
    gl_->images.draw(&instance, 1, sizeof(ImageInstance), buffer_width_, buffer_height_);
}

float Renderer::draw_text(std::string_view utf8, float x, float y, float size, Color color,
                          float max_width, Ellipsis ellipsis, Typeface typeface) {
    const TextLayout& layout = fonts_.layout(utf8, size, scale_, max_width, ellipsis, typeface);
    draw_layout(layout, x, y, color);
    return layout.width / scale_;
}

float Renderer::draw_text_runs(std::string_view utf8, float x, float y, float size, Color color,
                               std::span<const ColorRun> runs, float max_width, Ellipsis ellipsis,
                               Typeface typeface) {
    const TextLayout& layout = fonts_.layout(utf8, size, scale_, max_width, ellipsis, typeface);
    draw_layout(layout, x, y, color, runs);
    return layout.width / scale_;
}

void Renderer::draw_layout(const TextLayout& layout, float x, float y, Color color,
                           std::span<const ColorRun> runs) {
    // The line starts on a whole pixel; each glyph keeps its fractional position in quarter
    // pixels (subpixel positioning, as Pango and Qt), rasterized at that offset, so spacing stays
    // as the font and its tracking say. Bitmaps still map 1:1 onto the screen.
    constexpr float kSubpixelSteps = 4.0F;
    const float origin_x = snap(x, scale_);
    const float baseline = snap(y, scale_) + std::round(layout.ascent);
    const Color premultiplied = color.premultiplied();
    const auto size_64 = static_cast<std::uint32_t>(std::lround(layout.pixel_size * 64.0F));
    // The colour of the glyph from byte cluster: the run holding it, else color.
    const auto colour_of = [&](std::uint32_t cluster) {
        const auto after = std::ranges::upper_bound(runs, cluster, {}, &ColorRun::start);
        if (after == runs.begin() || cluster >= std::prev(after)->end) {
            return premultiplied;
        }
        return std::prev(after)->color.premultiplied();
    };

    for (const ShapedGlyph& glyph : layout.glyphs) {
        const float pen_x = origin_x + glyph.x;
        float whole = std::floor(pen_x);
        auto step = static_cast<std::uint32_t>(std::lround((pen_x - whole) * kSubpixelSteps));
        if (step >= static_cast<std::uint32_t>(kSubpixelSteps)) {
            step = 0;
            whole += 1.0F;
        }
        const GlyphKey key{glyph.font, glyph.glyph, size_64, step};
        auto it = gl_->entries.find(key);
        if (it == gl_->entries.end()) {
            it = gl_->entries
                     .emplace(key, rasterize(glyph, layout.pixel_size,
                                             static_cast<float>(step) / kSubpixelSteps))
                     .first;
        }
        const GlyphEntry& entry = it->second;
        if (entry.width == 0) {
            continue;  // blank glyph (space) or failed raster
        }
        const float gx = whole + static_cast<float>(entry.left);
        const float gy = std::round(baseline + glyph.y) - static_cast<float>(entry.top);
        const Color shade = runs.empty() ? premultiplied : colour_of(glyph.cluster);
        push_glyph({
            .rect = {gx, gy, static_cast<float>(entry.width), static_cast<float>(entry.height)},
            .uv = entry.uv,
            .color = {shade.r, shade.g, shade.b, shade.a},
            .kind = entry.color ? 1.0F : 0.0F,
        });
    }
}

// Rasterizes a glyph into its atlas. When an atlas is full, pending glyphs are drawn and the
// whole cache starts over: simple, and rare with 2048x2048 atlases.
Renderer::GlyphEntry Renderer::rasterize(const ShapedGlyph& glyph, float pixel_size,
                                         float x_offset) {
    const GlyphBitmap bitmap = fonts_.rasterize(glyph.font, glyph.glyph, pixel_size, x_offset);
    GlyphEntry entry{.left = bitmap.left, .top = bitmap.top, .color = bitmap.color};
    if (bitmap.width == 0 || bitmap.height == 0) {
        return entry;
    }
    Atlas& atlas = bitmap.color ? gl_->color_atlas : gl_->coverage_atlas;
    std::int32_t x = 0;
    std::int32_t y = 0;
    if (!atlas.insert(bitmap.width, bitmap.height, bitmap.pixels.data(), x, y)) {
        log::debug("glyph atlas full: flushing and starting over");
        flush();
        gl_->entries.clear();
        gl_->coverage_atlas.clear();
        gl_->color_atlas.clear();
        if (!atlas.insert(bitmap.width, bitmap.height, bitmap.pixels.data(), x, y)) {
            return entry;  // larger than the whole atlas
        }
    }
    constexpr auto atlas_size = static_cast<float>(kAtlasSize);
    entry.width = bitmap.width;
    entry.height = bitmap.height;
    entry.uv = {static_cast<float>(x) / atlas_size, static_cast<float>(y) / atlas_size,
                static_cast<float>(x + bitmap.width) / atlas_size,
                static_cast<float>(y + bitmap.height) / atlas_size};
    return entry;
}

void Renderer::push_box(const BoxInstance& instance) {
    if (!glyphs_.empty()) {
        flush();  // keep paint order between text and boxes
    }
    boxes_.push_back(instance);
    if (boxes_.size() == kMaxInstancesPerDraw) {
        flush();
    }
}

void Renderer::push_glyph(const GlyphInstance& instance) {
    if (!boxes_.empty()) {
        flush();
    }
    glyphs_.push_back(instance);
    if (glyphs_.size() == kMaxInstancesPerDraw) {
        flush();
    }
}

void Renderer::push_clip(const Rect& rect) {
    flush();
    clips_.push_back(clips_.empty() ? rect : clips_.back().intersect(rect));
    apply_clip();
}

void Renderer::pop_clip() {
    if (clips_.empty()) {
        log::warn("renderer: pop_clip without push_clip");
        return;
    }
    flush();
    clips_.pop_back();
    apply_clip();
}

void Renderer::apply_clip() {
    if (clips_.empty()) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    const Rect& clip = clips_.back();
    const auto x0 = static_cast<GLint>(snap(clip.x, scale_));
    const auto y0 = static_cast<GLint>(snap(clip.y, scale_));
    const auto x1 = static_cast<GLint>(snap(clip.right(), scale_));
    const auto y1 = static_cast<GLint>(snap(clip.bottom(), scale_));
    glEnable(GL_SCISSOR_TEST);
    // GL's scissor origin is the bottom-left corner.
    glScissor(x0, buffer_height_ - y1, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

void Renderer::flush() {
    if (!boxes_.empty()) {
        gl_->boxes.draw(boxes_.data(), boxes_.size(), sizeof(BoxInstance), buffer_width_,
                        buffer_height_);
        boxes_.clear();
    }
    if (!glyphs_.empty()) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, gl_->coverage_atlas.texture());
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, gl_->color_atlas.texture());
        glActiveTexture(GL_TEXTURE0);
        gl_->glyphs.draw(glyphs_.data(), glyphs_.size(), sizeof(GlyphInstance), buffer_width_,
                         buffer_height_);
        glyphs_.clear();
    }
}

std::size_t Renderer::atlas_bytes() noexcept {
    constexpr auto texels = static_cast<std::size_t>(kAtlasSize) * kAtlasSize;
    return texels * (1 + 4);  // R8 coverage, RGBA8 color
}

}  // namespace nordend::ui
