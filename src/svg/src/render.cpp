#include "internal.hpp"
#include "rummage/svg/svg.hpp"

namespace rummage::svg {
Document::Document(std::string_view input) : nodes(xml(input)), sheet(stylesheet(nodes)) {
    styles.resize(nodes.size());
    paths.resize(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        auto id = nodes[i].attr("id");
        if (!id.empty())
            ids.try_emplace(id, i);
        const Style parent = nodes[i].parent < nodes.size() ? styles[nodes[i].parent] : Style{};
        styles[i] = style(nodes[i], parent, sheet, style_work);
    }
    const auto box = numbers(nodes[0].attr("viewBox"));
    require(box.empty() || (box.size() == 4 && box[2] > 0 && box[3] > 0));
    width = length(nodes[0].attr("width"), box.empty() ? 0 : box[2], box.empty() ? 0 : box[2]);
    height = length(nodes[0].attr("height"), box.empty() ? 0 : box[3], box.empty() ? 0 : box[3]);
    require(width > 0 && height > 0);
}

namespace {
struct Layer {
    Document& doc;
    Canvas canvas;
    std::size_t bytes{};

    Layer(Document& document, const Canvas& target, int pad = 0) : doc(document) {
        require(pad >= 0 && pad <= 384);
        canvas.image.width = target.image.width + static_cast<unsigned>(2 * pad);
        canvas.image.height = target.image.height + static_cast<unsigned>(2 * pad);
        canvas.x = target.x - pad;
        canvas.y = target.y - pad;
        require(canvas.image.width <= 2048 && canvas.image.height <= 2048);
        bytes = static_cast<std::size_t>(canvas.image.width) * canvas.image.height * 4;
        require(doc.live_layers < 16 &&
                doc.layer_bytes + doc.texture_bytes + bytes <= 64U * 1024U * 1024U);
        canvas.image.rgba.resize(bytes);
        ++doc.live_layers;
        doc.layer_bytes += bytes;
    }

    ~Layer() {
        --doc.live_layers;
        doc.layer_bytes -= bytes;
    }
};

struct Reference {
    Document& doc;
    std::size_t index;

    Reference(Document& d, std::size_t i) : doc(d), index(i) {
        require(doc.active.size() < kMaxDepth && doc.active.insert(index).second);
    }

    ~Reference() { doc.active.erase(index); }
};

void charge(Canvas& canvas, std::size_t work) {
    canvas.work += work;
    require(canvas.work <= 64U * 1024U * 1024U);
}

void over(Canvas& target, const Canvas& source, double opacity) {
    charge(target, source.work + target.image.rgba.size() / 4);
    const int left = std::max(target.x, source.x), top = std::max(target.y, source.y);
    const int right = std::min(target.x + static_cast<int>(target.image.width),
                               source.x + static_cast<int>(source.image.width));
    const int bottom = std::min(target.y + static_cast<int>(target.image.height),
                                source.y + static_cast<int>(source.image.height));
    for (int y = top; y < bottom; ++y)
        for (int x = left; x < right; ++x) {
            const auto i = (static_cast<std::size_t>(y - target.y) * target.image.width +
                            static_cast<unsigned>(x - target.x)) *
                           4;
            const auto j = (static_cast<std::size_t>(y - source.y) * source.image.width +
                            static_cast<unsigned>(x - source.x)) *
                           4;
            if (source.image.rgba[j + 3] == 0)
                continue;
            const double alpha = source.image.rgba[j + 3] * opacity / 255;
            for (std::size_t c = 0; c < 4; ++c)
                target.image.rgba[i + c] = channel(source.image.rgba[j + c] * opacity +
                                                   target.image.rgba[i + c] * (1 - alpha));
        }
}

void multiply(Canvas& target, const Canvas& mask, bool luminance) {
    charge(target, mask.work + target.image.rgba.size() / 4);
    for (std::size_t i = 0; i < target.image.rgba.size(); i += 4) {
        const double alpha = mask.image.rgba[i + 3] / 255.0;
        double factor = alpha;
        if (luminance && alpha > 0) {
            const double red = mask.image.rgba[i] / (255 * alpha),
                         green = mask.image.rgba[i + 1] / (255 * alpha),
                         blue = mask.image.rgba[i + 2] / (255 * alpha);
            factor = (.2126 * red + .7152 * green + .0722 * blue) * alpha;
        }
        for (std::size_t c = 0; c < 4; ++c)
            target.image.rgba[i + c] = channel(target.image.rgba[i + c] * factor);
    }
}

std::optional<std::size_t> reference(const Document& doc, std::string_view value) {
    value = trim(value);
    if (!value.starts_with("url("))
        return {};
    const auto end = value.find(')');
    if (end == value.npos)
        return {};
    auto id = trim(value.substr(4, end - 4));
    if (id.size() > 1 &&
        ((id.front() == '\'' && id.back() == '\'') || (id.front() == '"' && id.back() == '"')))
        id = id.substr(1, id.size() - 2);
    if (!id.starts_with('#'))
        return {};
    auto it = doc.ids.find(id.substr(1));
    return it == doc.ids.end() ? std::nullopt : std::optional{it->second};
}

struct Engine {
    Document& doc;
    Raster raster;
    std::vector<std::pair<double, double>> path_extents;
    std::size_t bounds_work{};

    explicit Engine(Document& d) : doc(d), path_extents(d.nodes.size()) {}

    const Path& path(std::size_t index, double w, double h) {
        if (!doc.paths[index] || path_extents[index] != std::pair{w, h}) {
            doc.paths[index] = geometry(doc.nodes[index], w, h, doc.segments);
            path_extents[index] = {w, h};
        }
        return *doc.paths[index];
    }

    void viewport_clip(Canvas& canvas, const Matrix& matrix, double w, double h) {
        Layer mask{doc, canvas};
        Path rectangle{{{'M', {0, 0}, {}, {}},
                        {'L', {w, 0}, {}, {}},
                        {'L', {w, h}, {}, {}},
                        {'L', {0, h}, {}, {}},
                        {'Z', {}, {}, {}}}};
        Paint white;
        white.solid = {1, 1, 1, 1};
        raster.draw(mask.canvas, rectangle, matrix, white, 1, false, nullptr, w, h);
        multiply(canvas, mask.canvas, false);
    }

    std::optional<Box> box(std::size_t index, double w, double h, Matrix matrix = {},
                           std::size_t depth = 0, bool first = true, bool mapped = false) {
        require(depth < kMaxDepth && ++bounds_work <= kMaxSegments);
        const auto& node = doc.nodes[index];
        if (!doc.styles[index].display)
            return {};
        if (!first)
            matrix = matrix * transform(node.attr("transform"));
        if (node.name == "use") {
            auto href = node.attr("href");
            if (!href.starts_with('#'))
                return {};
            auto it = doc.ids.find(href.substr(1));
            if (it == doc.ids.end())
                return {};
            Reference guard{doc, it->second};
            matrix =
                matrix * Matrix{1, 0, 0, 1, length(node.attr("x"), w), length(node.attr("y"), h)};
            const auto& target = doc.nodes[it->second];
            if (target.name == "symbol" || target.name == "svg") {
                double vw = length(node.attr("width"), w, w),
                       vh = length(node.attr("height"), h, h);
                matrix = matrix * viewport(target.attr("viewBox"),
                                           target.attr("preserveAspectRatio"), vw, vh);
                auto vb = numbers(target.attr("viewBox"));
                w = vb.empty() ? vw : vb[2];
                h = vb.empty() ? vh : vb[3];
            }
            return box(it->second, w, h, matrix, depth + 1, false, true);
        }
        if (node.name == "g" || node.name == "svg" || node.name == "symbol" ||
            node.name == "clipPath" || node.name == "mask") {
            if (node.name == "svg" && !first && !mapped) {
                double vw = length(node.attr("width"), w, w),
                       vh = length(node.attr("height"), h, h);
                matrix = matrix *
                         Matrix{1, 0, 0, 1, length(node.attr("x"), w), length(node.attr("y"), h)} *
                         viewport(node.attr("viewBox"), node.attr("preserveAspectRatio"), vw, vh);
                auto vb = numbers(node.attr("viewBox"));
                w = vb.empty() ? vw : vb[2];
                h = vb.empty() ? vh : vb[3];
            }
            std::optional<Box> result;
            for (auto child : node.children)
                if (auto b = box(child, w, h, matrix, depth + 1, false)) {
                    if (!result)
                        result = b;
                    else {
                        double right = std::max(result->x + result->width, b->x + b->width),
                               bottom = std::max(result->y + result->height, b->y + b->height);
                        result->x = std::min(result->x, b->x);
                        result->y = std::min(result->y, b->y);
                        result->width = right - result->x;
                        result->height = bottom - result->y;
                    }
                }
            return result;
        }
        if (node.name == "image") {
            const double x = length(node.attr("x"), w), y = length(node.attr("y"), h),
                         iw = length(node.attr("width"), w), ih = length(node.attr("height"), h);
            Path shape{{{'M', {x, y}, {}, {}},
                        {'L', {x + iw, y}, {}, {}},
                        {'L', {x + iw, y + ih}, {}, {}},
                        {'L', {x, y + ih}, {}, {}}}};
            for (auto& point : shape.segments)
                point.p = matrix.apply(point.p);
            return bounds(shape);
        }
        const auto& original = path(index, w, h);
        if (original.segments.empty())
            return {};
        Path converted = original;
        for (auto& s : converted.segments) {
            s.p = matrix.apply(s.p);
            s.c1 = matrix.apply(s.c1);
            s.c2 = matrix.apply(s.c2);
            for (auto p : {s.p, s.c1, s.c2})
                require(std::isfinite(p.x) && std::isfinite(p.y));
        }
        return bounds(converted);
    }

    void clip(Canvas& canvas, std::size_t index, const Box& bbox, const Matrix& matrix, double w,
              double h, std::size_t depth) {
        if (doc.nodes[index].name != "clipPath")
            return;
        Reference guard{doc, index};
        Layer mask{doc, canvas};
        Matrix units = matrix;
        if (doc.nodes[index].attr("clipPathUnits") == "objectBoundingBox") {
            if (bbox.width <= 0 || bbox.height <= 0) {
                std::fill(canvas.image.rgba.begin(), canvas.image.rgba.end(), 0);
                return;
            }
            units = units * Matrix{bbox.width, 0, 0, bbox.height, bbox.x, bbox.y};
            w = h = 1;
        }
        draw(index, {}, units, w, h, mask.canvas, depth + 1, false, true, true);
        multiply(canvas, mask.canvas, false);
    }

    void mask(Canvas& canvas, std::size_t index, const Box& bbox, const Matrix& matrix, double w,
              double h, std::size_t depth) {
        const auto& node = doc.nodes[index];
        if (node.name != "mask")
            return;
        Reference guard{doc, index};
        Layer coverage{doc, canvas};
        const bool object = node.attr("maskUnits") != "userSpaceOnUse";
        double x = length(node.attr("x"), object ? 1 : w, object ? -.1 : -.1 * w),
               y = length(node.attr("y"), object ? 1 : h, object ? -.1 : -.1 * h);
        double rw = length(node.attr("width"), object ? 1 : w, object ? 1.2 : 1.2 * w),
               rh = length(node.attr("height"), object ? 1 : h, object ? 1.2 : 1.2 * h);
        if (object) {
            x = bbox.x + x * bbox.width;
            y = bbox.y + y * bbox.height;
            rw *= bbox.width;
            rh *= bbox.height;
        }
        if (rw <= 0 || rh <= 0) {
            std::fill(canvas.image.rgba.begin(), canvas.image.rgba.end(), 0);
            return;
        }
        Matrix content = matrix;
        double cw = w, ch = h;
        if (node.attr("maskContentUnits") == "objectBoundingBox") {
            content = content * Matrix{bbox.width, 0, 0, bbox.height, bbox.x, bbox.y};
            cw = ch = 1;
        }
        draw(index, {}, content, cw, ch, coverage.canvas, depth + 1, false, true, false);
        viewport_clip(coverage.canvas, matrix * Matrix{1, 0, 0, 1, x, y}, rw, rh);
        multiply(canvas, coverage.canvas, doc.styles[index].mask_type != "alpha");
    }

    struct Resource {
        Node node;
        std::size_t content;
    };

    Resource resource(std::size_t index, std::string_view kind) {
        // Only attributes and children participate in resource inheritance.
        // Do not repeatedly copy arbitrary text stored inside a definition.
        Resource result{{}, index};
        std::set<std::size_t> chain;
        for (;;) {
            require(chain.size() < kMaxDepth && chain.insert(index).second);
            const auto& node = doc.nodes[index];
            doc.paint_work += node.attributes.size() + node.children.size();
            require(doc.paint_work <= kMaxSegments);
            for (const auto& [k, v] : node.attributes) {
                doc.charge_resource(k.size() + v.size());
                result.node.attributes.try_emplace(k, v);
            }
            if (result.node.children.empty() && !node.children.empty()) {
                result.node.children = node.children;
                result.content = index;
            }
            const auto href = node.attr("href");
            if (!href.starts_with('#'))
                break;
            const auto it = doc.ids.find(href.substr(1));
            if (it == doc.ids.end() || doc.nodes[it->second].name != kind)
                break;
            index = it->second;
        }
        return result;
    }

    Paint paint(std::string_view value, Colour current, const Box& bbox, const Matrix& matrix,
                double w, double h, Canvas& canvas, std::size_t depth) {
        doc.charge_resource(value.size());
        const auto ref = reference(doc, value);
        if (!ref || doc.nodes[*ref].name != "pattern")
            return doc.paint(value, current, bbox, matrix, w, h);
        Reference guard{doc, *ref};
        const auto pattern = resource(*ref, "pattern");
        const auto& node = pattern.node;
        const bool object = node.attr("patternUnits") != "userSpaceOnUse";
        const double ex = object ? 1 : w, ey = object ? 1 : h;
        const double x = length(node.attr("x"), ex), y = length(node.attr("y"), ey),
                     tw = length(node.attr("width"), ex), th = length(node.attr("height"), ey);
        Paint out;
        if (tw <= 0 || th <= 0 || (object && (bbox.width <= 0 || bbox.height <= 0)))
            return out;
        const Matrix units =
            object ? Matrix{bbox.width, 0, 0, bbox.height, bbox.x, bbox.y} : Matrix{};
        const Matrix device = matrix * units * transform(node.attr("patternTransform"));
        const auto inverse = device.inverse();
        if (!inverse)
            return out;
        const double dw = std::ceil(tw * std::hypot(device.a, device.b)),
                     dh = std::ceil(th * std::hypot(device.c, device.d));
        require(std::isfinite(dw) && std::isfinite(dh) && dw <= 1024 && dh <= 1024);
        Canvas size;
        size.image.width = static_cast<std::uint32_t>(std::max(1.0, dw));
        size.image.height = static_cast<std::uint32_t>(std::max(1.0, dh));
        Layer tile{doc, size};
        const Matrix scale{size.image.width / tw, 0, 0, size.image.height / th, 0, 0};
        Matrix content = scale * Matrix{1, 0, 0, 1, -x, -y};
        const auto vb = numbers(node.attr("viewBox"));
        double cw = w, ch = h;
        if (!vb.empty()) {
            content = viewport(node.attr("viewBox"), node.attr("preserveAspectRatio"),
                               size.image.width, size.image.height);
            require(vb.size() == 4);
            cw = vb[2];
            ch = vb[3];
        } else {
            const Matrix bbox_units{bbox.width, 0, 0, bbox.height, bbox.x, bbox.y};
            if (node.attr("patternContentUnits") == "objectBoundingBox") {
                if (!object)
                    content = content * bbox_units;
                cw = ch = 1;
            } else if (object) {
                const auto inv = bbox_units.inverse();
                if (!inv)
                    return out;
                content = content * (*inv);
            }
        }
        for (auto child : node.children)
            draw(child, doc.styles[pattern.content], content, cw, ch, tile.canvas, depth + 1);
        charge(canvas, tile.canvas.work);
        doc.texture_bytes += tile.canvas.image.rgba.size();
        require(doc.texture_bytes + doc.layer_bytes <= 64U * 1024U * 1024U);
        out.texture = std::make_shared<picture::Image>(std::move(tile.canvas.image));
        out.inverse = scale * Matrix{1, 0, 0, 1, -x, -y} * (*inverse);
        out.repeat = true;
        return out;
    }

    void image(std::size_t index, const Matrix& matrix, double w, double h, Canvas& canvas) {
        const auto& node = doc.nodes[index];
        const auto pixels = embedded(doc, node.attr("href"));
        if (!pixels)
            return;
        const double x = length(node.attr("x"), w), y = length(node.attr("y"), h),
                     width = length(node.attr("width"), w), height = length(node.attr("height"), h);
        if (width <= 0 || height <= 0)
            return;
        const std::string vb =
            "0 0 " + std::to_string(pixels->width) + " " + std::to_string(pixels->height);
        const Matrix placed = matrix * Matrix{1, 0, 0, 1, x, y};
        const auto inverse =
            (placed * viewport(vb, node.attr("preserveAspectRatio"), width, height)).inverse();
        if (!inverse)
            return;
        Paint texture;
        texture.texture = pixels;
        texture.inverse = *inverse;
        Path rectangle{{{'M', {0, 0}, {}, {}},
                        {'L', {width, 0}, {}, {}},
                        {'L', {width, height}, {}, {}},
                        {'L', {0, height}, {}, {}},
                        {'Z', {}, {}, {}}}};
        raster.draw(canvas, rectangle, placed, texture, 1, false, nullptr, w, h);
    }

    Box region(const Node& node, std::string_view units, const Box& bbox, double w, double h,
               std::optional<Box> defaults = {}) {
        const bool object = units == "objectBoundingBox";
        const auto fallback = defaults.value_or(object ? Box{-.1, -.1, 1.2, 1.2}
                                                       : Box{-.1 * w, -.1 * h, 1.2 * w, 1.2 * h});
        Box b{length(node.attr("x"), object ? 1 : w, fallback.x),
              length(node.attr("y"), object ? 1 : h, fallback.y),
              length(node.attr("width"), object ? 1 : w, fallback.width),
              length(node.attr("height"), object ? 1 : h, fallback.height)};
        if (object)
            b = {bbox.x + b.x * bbox.width, bbox.y + b.y * bbox.height, b.width * bbox.width,
                 b.height * bbox.height};
        return b;
    }

    std::pair<double, double> sigma(const Node& node, bool object, const Box& bbox,
                                    const Matrix& matrix) {
        const auto values = numbers(node.attr("stdDeviation"));
        require(values.size() <= 2);
        double x = values.empty() ? 0 : values[0], y = values.size() == 2 ? values[1] : x;
        require(x >= 0 && y >= 0);
        if (object) {
            x *= bbox.width;
            y *= bbox.height;
        }
        // Axis-aligned device-space approximation for transformed Gaussian kernels.
        x *= std::hypot(matrix.a, matrix.b);
        y *= std::hypot(matrix.c, matrix.d);
        require(std::isfinite(x) && std::isfinite(y));
        return {std::min(32.0, x), std::min(32.0, y)};
    }

    std::optional<Resource> filter(std::optional<std::size_t> index) {
        if (!index || doc.nodes[*index].name != "filter")
            return {};
        auto result = resource(*index, "filter");
        for (auto child : result.node.children) {
            auto name = doc.nodes[child].name;
            if (name != "feColorMatrix" && name != "feGaussianBlur") {
                doc.warning();
                return {};
            }
        }
        return result;
    }

    int padding(const Resource& filter, const Box& bbox, const Matrix& matrix) {
        double pad = 0;
        for (auto child : filter.node.children)
            if (doc.nodes[child].name == "feGaussianBlur") {
                const auto [x, y] =
                    sigma(doc.nodes[child],
                          filter.node.attr("primitiveUnits") == "objectBoundingBox", bbox, matrix);
                pad += 3 * std::max(x, y);
            }
        require(pad <= 384);
        return static_cast<int>(std::ceil(pad));
    }

    void apply_filter(Canvas& canvas, const Resource& filter, const Box& bbox, const Matrix& matrix,
                      double w, double h) {
        const auto& node = filter.node;
        const auto area = region(
            node,
            node.attr("filterUnits") == "userSpaceOnUse" ? "userSpaceOnUse" : "objectBoundingBox",
            bbox, w, h);
        if (area.width <= 0 || area.height <= 0) {
            std::fill(canvas.image.rgba.begin(), canvas.image.rgba.end(), 0);
            return;
        }
        std::vector<std::unique_ptr<Layer>> layers;
        std::map<std::string_view, Canvas*> results;
        Canvas* previous = &canvas;
        Canvas* alpha = nullptr;
        auto layer = [&]() {
            layers.push_back(std::make_unique<Layer>(doc, canvas));
            return &layers.back()->canvas;
        };
        for (auto child : node.children) {
            const auto& primitive = doc.nodes[child];
            const auto in = primitive.attr("in");
            Canvas* input = previous;
            if (in == "SourceGraphic")
                input = &canvas;
            else if (in == "SourceAlpha") {
                if (!alpha) {
                    alpha = layer();
                    for (std::size_t i = 3; i < canvas.image.rgba.size(); i += 4)
                        alpha->image.rgba[i] = canvas.image.rgba[i];
                }
                input = alpha;
            } else if (!in.empty()) {
                const auto it = results.find(in);
                if (it == results.end())
                    input = layer();
                else
                    input = it->second;
            }
            Canvas* output = layer();
            const bool linear = doc.styles[child].filter_space != "sRGB";
            if (primitive.name == "feColorMatrix")
                colour_matrix(*input, *output, primitive, linear);
            else {
                const auto [sx, sy] = sigma(
                    primitive, node.attr("primitiveUnits") == "objectBoundingBox", bbox, matrix);
                Layer scratch{doc, canvas};
                blur(*input, *output, scratch.canvas, sx, sy, linear);
            }
            charge(canvas,
                   canvas.image.rgba.size() / 4 * (primitive.name == "feGaussianBlur" ? 8U : 1U));
            Box sub = area;
            if (!primitive.attr("x").empty() || !primitive.attr("y").empty() ||
                !primitive.attr("width").empty() || !primitive.attr("height").empty()) {
                const bool object = node.attr("primitiveUnits") == "objectBoundingBox";
                Box defaults = area;
                if (object && bbox.width > 0 && bbox.height > 0)
                    defaults = {(area.x - bbox.x) / bbox.width, (area.y - bbox.y) / bbox.height,
                                area.width / bbox.width, area.height / bbox.height};
                sub = region(primitive, object ? "objectBoundingBox" : "userSpaceOnUse", bbox, w, h,
                             defaults);
            }
            if (sub.width > 0 && sub.height > 0)
                viewport_clip(*output, matrix * Matrix{1, 0, 0, 1, sub.x, sub.y}, sub.width,
                              sub.height);
            else
                std::fill(output->image.rgba.begin(), output->image.rgba.end(), 0);
            charge(canvas, output->work);
            if (!primitive.attr("result").empty())
                results[primitive.attr("result")] = output;
            previous = output;
        }
        if (previous != &canvas)
            canvas.image.rgba = previous->image.rgba;
        viewport_clip(canvas, matrix * Matrix{1, 0, 0, 1, area.x, area.y}, area.width, area.height);
    }

    void content(std::size_t index, const Style& st, const Matrix& matrix, double w, double h,
                 Canvas& canvas, std::size_t depth, bool instance, bool referenced, bool clipping) {
        const auto& node = doc.nodes[index];
        if (node.name == "use") {
            auto href = node.attr("href");
            if (!href.starts_with('#'))
                return;
            auto it = doc.ids.find(href.substr(1));
            if (it == doc.ids.end())
                return;
            Reference guard{doc, it->second};
            const Matrix placement =
                matrix * Matrix{1, 0, 0, 1, length(node.attr("x"), w), length(node.attr("y"), h)};
            const auto& target = doc.nodes[it->second];
            if (target.name == "symbol" || target.name == "svg") {
                const double vw = length(node.attr("width"), w, w),
                             vh = length(node.attr("height"), h, h);
                require(vw >= 0 && vh >= 0);
                if (vw == 0 || vh == 0)
                    return;
                const Matrix mapped =
                    placement *
                    viewport(target.attr("viewBox"), target.attr("preserveAspectRatio"), vw, vh);
                auto vb = numbers(target.attr("viewBox"));
                Layer layer{doc, canvas};
                draw(it->second, st, mapped, vb.empty() ? vw : vb[2], vb.empty() ? vh : vb[3],
                     layer.canvas, depth + 1, true, true, clipping);
                viewport_clip(layer.canvas, placement, vw, vh);
                over(canvas, layer.canvas, 1);
            } else
                draw(it->second, st, placement, w, h, canvas, depth + 1, true, true, clipping);
            return;
        }
        if (node.name == "g" || node.name == "svg" ||
            (referenced &&
             (node.name == "symbol" || node.name == "mask" || node.name == "clipPath"))) {
            if (node.name == "svg" && index != 0 && !referenced) {
                const double vw = length(node.attr("width"), w, w),
                             vh = length(node.attr("height"), h, h);
                require(vw >= 0 && vh >= 0);
                if (vw == 0 || vh == 0)
                    return;
                const Matrix placement =
                    matrix *
                    Matrix{1, 0, 0, 1, length(node.attr("x"), w), length(node.attr("y"), h)};
                const Matrix mapped =
                    placement *
                    viewport(node.attr("viewBox"), node.attr("preserveAspectRatio"), vw, vh);
                auto vb = numbers(node.attr("viewBox"));
                Layer layer{doc, canvas};
                for (auto child : node.children)
                    draw(child, st, mapped, vb.empty() ? vw : vb[2], vb.empty() ? vh : vb[3],
                         layer.canvas, depth + 1, instance, false, clipping);
                viewport_clip(layer.canvas, placement, vw, vh);
                over(canvas, layer.canvas, 1);
            } else
                for (auto child : node.children)
                    draw(child, st, matrix, w, h, canvas, depth + 1, instance, false, clipping);
            return;
        }
        if (!st.visible)
            return;
        if (node.name == "image") {
            if (!clipping)
                image(index, matrix, w, h, canvas);
            return;
        }
        const auto& shape = path(index, w, h);
        if (shape.segments.empty())
            return;
        if (clipping) {
            Paint white;
            white.solid = {1, 1, 1, 1};
            raster.draw(canvas, shape, matrix, white, 1, st.clip_rule == "evenodd", nullptr, w, h);
            return;
        }
        const auto bbox = bounds(shape);
        const auto fill = paint(st.fill, st.color, bbox, matrix, w, h, canvas, depth),
                   stroke = paint(st.stroke, st.color, bbox, matrix, w, h, canvas, depth);
        raster.draw(canvas, shape, matrix, fill, st.fill_opacity, st.fill_rule == "evenodd",
                    nullptr, w, h);
        raster.draw(canvas, shape, matrix, stroke, st.stroke_opacity, false, &st, w, h);
    }

    void draw(std::size_t index, const Style& parent, Matrix matrix, double w, double h,
              Canvas& canvas, std::size_t depth = 0, bool instance = false, bool referenced = false,
              bool clipping = false) {
        require(depth < kMaxDepth && ++doc.visited <= kMaxNodes + kMaxExpansion);
        if (instance)
            require(++doc.expansions <= kMaxExpansion);
        const auto& node = doc.nodes[index];
        const auto st =
            instance ? style(node, parent, doc.sheet, doc.style_work) : doc.styles[index];
        if (!st.display || (!clipping && st.opacity == 0))
            return;
        if (index != 0)
            matrix = matrix * transform(node.attr("transform"));
        if (!matrix.inverse())
            return;
        const auto clip_ref = reference(doc, st.clip),
                   mask_ref = clipping ? std::nullopt : reference(doc, st.mask);
        const double opacity = clipping ? 1 : st.opacity;
        const auto filter_ref = clipping ? std::nullopt : reference(doc, st.filter);
        const auto effect = filter(filter_ref);
        if (opacity < 1 || clip_ref || mask_ref || effect) {
            const auto bbox =
                (clip_ref || mask_ref || effect) ? box(index, w, h).value_or(Box{}) : Box{};
            std::unique_ptr<Reference> filter_guard;
            if (effect)
                filter_guard = std::make_unique<Reference>(doc, *filter_ref);
            Layer layer{doc, canvas, effect ? padding(*effect, bbox, matrix) : 0};
            content(index, st, matrix, w, h, layer.canvas, depth, instance, referenced, clipping);
            if (effect)
                apply_filter(layer.canvas, *effect, bbox, matrix, w, h);
            if (clip_ref)
                clip(layer.canvas, *clip_ref, bbox, matrix, w, h, depth);
            if (mask_ref)
                mask(layer.canvas, *mask_ref, bbox, matrix, w, h, depth);
            over(canvas, layer.canvas, opacity);
        } else
            content(index, st, matrix, w, h, canvas, depth, instance, referenced, clipping);
    }
};
}  // namespace

void Document::warning() {
    if (warned)
        return;
    warned = true;
    if (options.warning)
        options.warning(options.context, "unsupported SVG filter primitive; rendering unfiltered");
}

picture::Image render(std::span<const std::uint8_t> data, int pixels,
                      RenderOptions options) noexcept {
    try {
        require(!data.empty() && data.size() <= kMaxBytes && pixels > 0 && pixels <= 1024);
        Document doc{{reinterpret_cast<const char*>(data.data()), data.size()}};
        doc.options = options;
        const double scale = static_cast<double>(pixels) / std::max(doc.width, doc.height);
        require(std::isfinite(scale));
        Canvas canvas;
        canvas.image.width =
            static_cast<std::uint32_t>(std::max(1.0, std::round(doc.width * scale)));
        canvas.image.height =
            static_cast<std::uint32_t>(std::max(1.0, std::round(doc.height * scale)));
        canvas.image.rgba.resize(static_cast<std::size_t>(canvas.image.width) *
                                 canvas.image.height * 4);
        const auto& root = doc.nodes[0];
        Matrix matrix =
            Matrix{scale, 0, 0, scale, 0, 0} * transform(root.attr("transform")) *
            viewport(root.attr("viewBox"), root.attr("preserveAspectRatio"), doc.width, doc.height);
        auto vb = numbers(root.attr("viewBox"));
        Engine engine{doc};
        engine.draw(0, {}, matrix, vb.empty() ? doc.width : vb[2], vb.empty() ? doc.height : vb[3],
                    canvas);
        straighten(canvas.image);
        return std::move(canvas.image);
    } catch (...) {
        return {};
    }
}
}  // namespace rummage::svg
