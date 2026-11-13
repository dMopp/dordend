#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ft2build.h>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/svg/svg.hpp"
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_STROKER_H
#include "rummage/picture/image.hpp"

namespace rummage::svg {
inline constexpr std::size_t kMaxBytes = 8U * 1024U * 1024U, kMaxNodes = 100000, kMaxDepth = 256,
                             kMaxSegments = 1000000, kMaxExpansion = 10000;

struct Failure {};

inline std::uint8_t channel(double value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0, 255.0) + .5);
}

inline void require(bool value) {
    if (!value) {
        throw Failure{};
    }
}

struct Node {
    std::string name;
    std::map<std::string, std::string, std::less<>> attributes;
    std::vector<std::size_t> children;
    std::string text;
    std::size_t parent = std::numeric_limits<std::size_t>::max();

    std::string_view attr(std::string_view key) const {
        auto it = attributes.find(key);
        return it == attributes.end() ? std::string_view{} : it->second;
    }
};

std::vector<Node> xml(std::string_view input);
std::string_view trim(std::string_view input);

struct Numbers {
    std::string_view input;
    std::size_t at{};
    void skip();
    bool more();
    double get();
    bool flag();
};

double number(std::string_view input, double fallback = 0);
std::vector<double> numbers(std::string_view input);
double length(std::string_view input, double extent, double fallback = 0);

struct Point {
    double x{}, y{};

    Point operator+(Point b) const { return {x + b.x, y + b.y}; }

    Point operator-(Point b) const { return {x - b.x, y - b.y}; }

    Point operator*(double s) const { return {x * s, y * s}; }
};

struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    Point apply(Point p) const { return {a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }

    Matrix operator*(const Matrix& rhs) const;
    std::optional<Matrix> inverse() const;
};

Matrix transform(std::string_view text);
Matrix viewport(std::string_view viewbox, std::string_view aspect, double width, double height);
using Colour = std::array<double, 4>;
Colour colour(std::string_view text, Colour current = {0, 0, 0, 1});

struct Style {
    std::string_view fill = "black", stroke = "none", fill_rule = "nonzero", clip_rule = "nonzero",
                     linecap = "butt", linejoin = "miter";
    std::string_view dasharray = "none", stroke_width = "1", dashoffset = "0";
    std::string_view clip, mask, filter, mask_type = "luminance", filter_space = "linearRGB";
    Colour color{0, 0, 0, 1}, stop_color{0, 0, 0, 1};
    double opacity = 1, fill_opacity = 1, stroke_opacity = 1, miterlimit = 4, stop_opacity = 1;
    bool display = true, visible = true;
};

struct Rule {
    std::string_view selector;
    std::map<std::string_view, std::string_view> declarations;
    int specificity{};
};

struct Sheet {
    std::vector<Rule> rules;
    std::vector<std::string> storage;
};

Sheet stylesheet(const std::vector<Node>& nodes);
Style style(const Node& node, const Style& parent, const Sheet& sheet, std::size_t& work);

struct Segment {
    char kind{};
    Point p{}, c1{}, c2{};
};

struct Path {
    std::vector<Segment> segments;
};

struct Box {
    double x{}, y{}, width{}, height{};
};

Path geometry(const Node& node, double width, double height, std::size_t& segment_count);
Box bounds(const Path& path);

struct Stop {
    double offset{};
    Colour color{};
};

struct Paint {
    Colour solid{};
    std::shared_ptr<picture::Image> texture;
    bool repeat = false;
    std::vector<Stop> stops;
    Matrix inverse;
    Point start{}, end{};
    double r{}, fr{};
    bool radial = false;
    std::string_view spread = "pad";
    Colour sample(double x, double y) const;
};

struct Canvas {
    picture::Image image;
    int x{}, y{};
    std::size_t work{};
    bool exhausted{};
};

void composite(Canvas& canvas, int x, int y, int count, unsigned coverage, const Paint& paint,
               double opacity) noexcept;
void straighten(picture::Image& image);

struct Raster {
    FT_Library library{};
    FT_Stroker stroker{};
    Raster();
    ~Raster();
    Raster(const Raster&) = delete;
    Raster& operator=(const Raster&) = delete;
    void draw(Canvas& canvas, const Path& path, const Matrix& matrix, const Paint& paint,
              double opacity, bool evenodd, const Style* stroke, double width, double height);
};

struct Document {
    std::vector<Node> nodes;
    Sheet sheet;
    std::map<std::string_view, std::size_t> ids;
    std::vector<Style> styles;
    std::vector<std::optional<Path>> paths;
    std::size_t segments{}, expansions{}, style_work{}, paint_work{}, visited{}, live_layers{},
        layer_bytes{}, texture_bytes{}, resource_bytes{};
    RenderOptions options;
    bool warned{};
    std::set<std::size_t> active;
    double width{}, height{};
    explicit Document(std::string_view input);

    void charge_resource(std::size_t bytes) {
        require(bytes <= 64U * 1024U * 1024U - resource_bytes);
        resource_bytes += bytes;
    }

    void warning();
    Paint paint(std::string_view value, Colour current, const Box& box, const Matrix& matrix,
                double w, double h);
};

std::shared_ptr<picture::Image> embedded(Document& doc, std::string_view href);
void colour_matrix(const Canvas& input, Canvas& output, const Node& node, bool linear);
void blur(const Canvas& input, Canvas& output, Canvas& scratch, double sx, double sy, bool linear);
}  // namespace rummage::svg
