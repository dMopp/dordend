#include "internal.hpp"

namespace rummage::svg {
Colour Paint::sample(double x, double y) const {
    if (texture) {
        Point p = inverse.apply({x, y});
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            return {};
        const int w = static_cast<int>(texture->width), h = static_cast<int>(texture->height);
        if (repeat) {
            // Subtracting a rounded huge quotient can leave a remainder outside
            // the integer range. fmod bounds it before the pixel-index cast.
            p.x = std::fmod(p.x, w);
            p.y = std::fmod(p.y, h);
            if (p.x < 0)
                p.x += w;
            if (p.y < 0)
                p.y += h;
        } else if (p.x < 0 || p.y < 0 || p.x >= w || p.y >= h)
            return {};
        p.x -= .5;
        p.y -= .5;
        const int ix = static_cast<int>(std::floor(p.x)), iy = static_cast<int>(std::floor(p.y));
        const double fx = p.x - ix, fy = p.y - iy;
        Colour out{};
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const int px = repeat ? ((ix + dx) % w + w) % w : std::clamp(ix + dx, 0, w - 1);
                const int py = repeat ? ((iy + dy) % h + h) % h : std::clamp(iy + dy, 0, h - 1);
                const auto at =
                    (static_cast<std::size_t>(py) * texture->width + static_cast<unsigned>(px)) * 4;
                const double factor = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) / 255;
                for (std::size_t c = 0; c < 4; ++c)
                    out[c] += texture->rgba[at + c] * factor;
            }
        if (out[3] > 0)
            for (std::size_t c = 0; c < 3; ++c)
                out[c] /= out[3];
        return out;
    }
    if (stops.empty()) {
        return solid;
    }
    Point p = inverse.apply({x, y});
    double t = 0;
    if (!radial) {
        Point d = end - start;
        double denominator = d.x * d.x + d.y * d.y;
        t = denominator > 1e-30 ? ((p.x - start.x) * d.x + (p.y - start.y) * d.y) / denominator : 1;
    } else {
        Point q = p - start, d = end - start;
        double dr = r - fr;
        const double a = d.x * d.x + d.y * d.y - dr * dr,
                     b = -2 * (q.x * d.x + q.y * d.y + fr * dr),
                     c = q.x * q.x + q.y * q.y - fr * fr;
        if (std::abs(a) < 1e-20) {
            if (std::abs(b) < 1e-20) {
                return {};
            }
            t = -c / b;
        } else {
            double disc = b * b - 4 * a * c;
            if (disc < 0) {
                return {};
            }
            double root = std::sqrt(disc);
            double t1 = (-b - root) / (2 * a), t2 = (-b + root) / (2 * a);
            t = std::max(t1, t2);
            if (fr + t * dr < 0) {
                t = std::min(t1, t2);
            }
        }
        if (fr + t * dr < 0) {
            return {};
        }
    }
    if (!std::isfinite(t)) {
        return {};
    }
    if (spread == "repeat") {
        t -= std::floor(t);
    } else if (spread == "reflect") {
        t = std::fmod(t, 2);
        if (t < 0) {
            t += 2;
        }
        if (t > 1) {
            t = 2 - t;
        }
    } else {
        t = std::clamp(t, 0.0, 1.0);
    }
    if (t < stops.front().offset) {
        return stops.front().color;
    }
    auto right =
        std::upper_bound(stops.begin(), stops.end(), t,
                         [](double value, const Stop& stop) { return value < stop.offset; });
    if (right == stops.end()) {
        return stops.back().color;
    }
    if (right == stops.begin()) {
        return right->color;
    }
    const auto& left = *(right - 1);
    const double ratio = (t - left.offset) / (right->offset - left.offset);
    Colour out{};
    // SVG interpolates unassociated colour and alpha separately.
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = left.color[i] + (right->color[i] - left.color[i]) * ratio;
    }
    return out;
}

void composite(Canvas& canvas, int x, int y, int count, unsigned coverage, const Paint& paint,
               double opacity) noexcept {
    if (y < 0 || y >= static_cast<int>(canvas.image.height)) {
        return;
    }
    int end = std::min(x + count, static_cast<int>(canvas.image.width));
    x = std::max(x, 0);
    if (end <= x || canvas.exhausted) {
        return;
    }
    canvas.work += static_cast<std::size_t>(end - x);
    if (canvas.work > 64U * 1024U * 1024U) {
        canvas.exhausted = true;
        return;
    }
    const double cov = opacity * coverage / 255;
    if (!paint.texture && paint.stops.empty()) {
        const double alpha = paint.solid[3] * cov, inverse = 1 - alpha;
        const Colour source{paint.solid[0] * alpha * 255, paint.solid[1] * alpha * 255,
                            paint.solid[2] * alpha * 255, alpha * 255};
        for (; x < end; ++x) {
            const auto at =
                (static_cast<std::size_t>(y) * canvas.image.width + static_cast<unsigned>(x)) * 4;
            for (std::size_t c = 0; c < 4; ++c)
                canvas.image.rgba[at + c] =
                    channel(source[c] + canvas.image.rgba[at + c] * inverse);
        }
        return;
    }
    for (; x < end; ++x) {
        auto color = paint.sample(x + canvas.x + .5, y + canvas.y + .5);
        double alpha = color[3] * cov;
        const auto at =
            (static_cast<std::size_t>(y) * canvas.image.width + static_cast<unsigned>(x)) * 4;
        for (std::size_t c = 0; c < 4; ++c) {
            double source = c == 3 ? alpha : color[c] * alpha;
            canvas.image.rgba[at + c] =
                channel(source * 255 + canvas.image.rgba[at + c] * (1 - alpha));
        }
    }
}

void straighten(picture::Image& image) {
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        unsigned alpha = image.rgba[i + 3];
        if (alpha == 0) {
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            image.rgba[i + c] = static_cast<std::uint8_t>(
                std::min(255U, (image.rgba[i + c] * 255U + alpha / 2U) / alpha));
        }
    }
}

Paint Document::paint(std::string_view value, Colour current, const Box& box, const Matrix& matrix,
                      double w, double h) {
    value = trim(value);
    Paint out;
    if (!value.starts_with("url(")) {
        out.solid = colour(value, current);
        return out;
    }
    const auto close = value.find(')');
    if (close == value.npos) {
        return out;
    }
    auto id = trim(value.substr(4, close - 4));
    if (id.size() > 1 &&
        ((id.front() == '\'' && id.back() == '\'') || (id.front() == '"' && id.back() == '"'))) {
        id = id.substr(1, id.size() - 2);
    }
    auto fallback = trim(value.substr(close + 1));
    if (!fallback.empty()) {
        out.solid = colour(fallback, current);
    }
    if (!id.starts_with('#')) {
        return out;
    }
    auto found = ids.find(id.substr(1));
    if (found == ids.end()) {
        return out;
    }
    std::size_t index = found->second;
    const bool radial = nodes[index].name == "radialGradient";
    if (!radial && nodes[index].name != "linearGradient") {
        return out;
    }
    std::map<std::string_view, std::string_view> attrs;
    std::vector<std::size_t> stop_nodes;
    std::set<std::size_t> gradient_chain;
    for (;;) {
        require(gradient_chain.size() < kMaxDepth && gradient_chain.insert(index).second);
        const auto& node = nodes[index];
        paint_work += node.attributes.size() + node.children.size();
        require(paint_work <= kMaxSegments);
        for (const auto& [key, v] : node.attributes) {
            charge_resource(key.size() + v.size());
            attrs.try_emplace(key, v);
        }
        if (stop_nodes.empty()) {
            for (auto child : node.children) {
                if (nodes[child].name == "stop") {
                    stop_nodes.push_back(child);
                }
            }
        }
        auto ref = node.attr("href");
        if (!ref.starts_with('#')) {
            break;
        }
        auto next = ids.find(ref.substr(1));
        if (next == ids.end()) {
            break;
        }
        if (nodes[next->second].name != "radialGradient" &&
            nodes[next->second].name != "linearGradient") {
            break;
        }
        index = next->second;
    }
    out.solid = {};
    out.radial = radial;
    auto attr = [&](std::string_view key, std::string_view initial = {}) {
        auto it = attrs.find(key);
        return it == attrs.end() ? initial : it->second;
    };
    const bool bbox = attr("gradientUnits") != "userSpaceOnUse";
    if (bbox && (box.width == 0 || box.height == 0)) {
        return out;
    }
    const Matrix units = bbox ? Matrix{box.width, 0, 0, box.height, box.x, box.y} : Matrix{};
    const auto inverse = (matrix * units * transform(attr("gradientTransform"))).inverse();
    if (!inverse) {
        return out;
    }
    out.inverse = *inverse;
    out.spread = attr("spreadMethod", "pad");
    const double ex = bbox ? 1 : w, ey = bbox ? 1 : h;
    if (radial) {
        out.end = {length(attr("cx", "50%"), ex), length(attr("cy", "50%"), ey)};
        out.start = {length(attr("fx"), ex, out.end.x), length(attr("fy"), ey, out.end.y)};
        const double diagonal = std::hypot(ex, ey) / std::sqrt(2.0);
        out.r = length(attr("r", "50%"), diagonal);
        out.fr = length(attr("fr"), diagonal);
        require(out.r >= 0 && out.fr >= 0);
    } else {
        out.start = {length(attr("x1", "0%"), ex), length(attr("y1", "0%"), ey)};
        out.end = {length(attr("x2", "100%"), ex), length(attr("y2", "0%"), ey)};
    }
    double previous = 0;
    for (auto child : stop_nodes) {
        const auto& st = styles[child];
        Colour color = st.stop_color;
        color[3] *= st.stop_opacity;
        const double offset = std::clamp(length(nodes[child].attr("offset"), 1), previous, 1.0);
        out.stops.push_back({offset, color});
        previous = offset;
    }
    if (out.stops.size() == 1 || (radial && out.r == 0)) {
        if (!out.stops.empty()) {
            out.solid = out.stops.back().color;
        }
        out.stops.clear();
    }
    return out;
}
}  // namespace rummage::svg
