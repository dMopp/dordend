#include <type_traits>

#include "internal.hpp"

namespace rummage::svg {
namespace {
using Tag = std::remove_pointer_t<decltype(FT_Outline::tags)>;
using Contour = std::remove_pointer_t<decltype(FT_Outline::contours)>;

FT_Pos fixed(double n) {
    require(std::isfinite(n) && std::abs(n) <= 32767);
    return static_cast<FT_Pos>(std::lround(n * 64));
}

FT_Vector vector(Point p) { return {fixed(p.x), fixed(p.y)}; }

FT_Vector high_vector(Point p) {
    static_cast<void>(vector(p));
    return {static_cast<FT_Pos>(p.x * 256), static_cast<FT_Pos>(p.y * 256)};
}

struct Outline {
    std::vector<FT_Vector> points;
    std::vector<Tag> tags;
    std::vector<Contour> contours;

    void point(Point p, Tag tag, bool high = false) {
        require(points.size() < 32767);
        points.push_back(high ? high_vector(p) : vector(p));
        tags.push_back(tag);
    }

    void close() {
        if (!points.empty() && (contours.empty() || contours.back() != points.size() - 1)) {
            contours.push_back(static_cast<Contour>(points.size() - 1));
        }
    }

    FT_Outline get(bool evenodd) {
        return {static_cast<decltype(FT_Outline::n_contours)>(contours.size()),
                static_cast<decltype(FT_Outline::n_points)>(points.size()),
                points.data(),
                tags.data(),
                contours.data(),
                FT_OUTLINE_OVERLAP | (evenodd ? FT_OUTLINE_EVEN_ODD_FILL : 0)};
    }
};

constexpr std::size_t kMaxRasterEdges = 256U * 1024U;

struct Edge {
    double x0, y0, x1, y1;
};

std::vector<double> curve_roots(std::array<double, 4> coefficients, int degree) {
    while (degree > 0 && std::abs(coefficients[static_cast<std::size_t>(degree)]) < 1e-12)
        --degree;
    if (degree == 0)
        return {};
    if (degree == 1) {
        const double t = -coefficients[0] / coefficients[1];
        return t > 1e-8 && t < 1 - 1e-8 ? std::vector<double>{t} : std::vector<double>{};
    }
    std::array<double, 4> derivative{};
    for (int i = 1; i <= degree; ++i)
        derivative[static_cast<std::size_t>(i - 1)] = i * coefficients[static_cast<std::size_t>(i)];
    auto intervals = curve_roots(derivative, degree - 1);
    intervals.insert(intervals.begin(), 0);
    intervals.push_back(1);
    auto value = [&](double t) {
        double n = coefficients[static_cast<std::size_t>(degree)];
        for (int i = degree - 1; i >= 0; --i)
            n = n * t + coefficients[static_cast<std::size_t>(i)];
        return n;
    };
    std::vector<double> result;
    for (std::size_t i = 1; i < intervals.size(); ++i) {
        double lo = intervals[i - 1], hi = intervals[i], vlo = value(lo), vhi = value(hi);
        if (lo > 1e-8 && std::abs(vlo) < 1e-9)
            result.push_back(lo);
        if ((vlo < 0) == (vhi < 0) || vlo == 0 || vhi == 0)
            continue;
        for (int n = 0; n < 45; ++n) {
            const double mid = (lo + hi) * .5, v = value(mid);
            if ((v < 0) == (vlo < 0)) {
                lo = mid;
                vlo = v;
            } else
                hi = mid;
        }
        const double t = (lo + hi) * .5;
        if (t > 1e-8 && t < 1 - 1e-8)
            result.push_back(t);
    }
    return result;
}

struct Decompose {
    std::vector<Edge> edges;
    Point current{}, start{};
    bool failed{}, clip{};
    double width{}, height{};

    void line(Point p) {
        require(edges.size() < kMaxRasterEdges);
        if (current.y != p.y)
            edges.push_back({current.x, current.y, p.x, p.y});
        current = p;
    }

    void quadratic(Point a, Point b, Point c, bool split = true) {
        if (split) {
            std::vector<double> cuts;
            auto axis = [&](double p, double q, double r, double extent, bool vertical) {
                if (vertical || clip) {
                    auto roots = curve_roots({q - p, p - 2 * q + r, 0, 0}, 1);
                    cuts.insert(cuts.end(), roots.begin(), roots.end());
                }
                if (clip)
                    for (double edge : {0.0, extent}) {
                        auto roots = curve_roots({p - edge, 2 * (q - p), p - 2 * q + r, 0}, 2);
                        cuts.insert(cuts.end(), roots.begin(), roots.end());
                    }
            };
            axis(a.x, b.x, c.x, width, false);
            axis(a.y, b.y, c.y, height, true);
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end(),
                                   [](double x, double y) { return std::abs(x - y) < 1e-8; }),
                       cuts.end());
            double previous = 0;
            for (double t : cuts) {
                const double u = (t - previous) / (1 - previous);
                const Point ab = a + (b - a) * u, bc = b + (c - b) * u, mid = ab + (bc - ab) * u;
                quadratic(a, ab, mid, false);
                a = mid;
                b = bc;
                previous = t;
            }
        }
        const Point delta = (b - (a + c) * .5) * .5;
        const double dx = std::abs(delta.x), dy = std::abs(delta.y);
        const double error = std::max(dx, dy) + .5 * std::min(dx, dy);
        unsigned steps = 2;
        while (steps < 64 && error >= .25 * steps * steps - .0625)
            steps *= 2;
        for (unsigned n = 1; n < steps; ++n) {
            const double t = static_cast<double>(n) / steps, u = 1 - t;
            Point p = a * (u * u) + b * (2 * u * t) + c * (t * t);
            p.x = std::floor(p.x * 256) / 256;
            p.y = std::floor(p.y * 256) / 256;
            line(p);
        }
        line(c);
    }

    // Split at vertical extrema, then sample each monotone cubic uniformly.
    // A power-of-two step count keeps neighbouring pieces stable as they scale.
    void curve(Point a, Point b, Point c, Point d, int depth = 0) {
        if (depth == 0) {
            std::vector<double> cuts;
            auto axis = [&](double p, double q, double r, double s, double extent, bool vertical) {
                const double a3 = -p + 3 * q - 3 * r + s, a2 = 3 * (p - 2 * q + r),
                             a1 = 3 * (q - p);
                if (vertical || clip) {
                    auto roots = curve_roots({a1, 2 * a2, 3 * a3, 0}, 2);
                    cuts.insert(cuts.end(), roots.begin(), roots.end());
                }
                if (clip)
                    for (double edge : {0.0, extent}) {
                        auto roots = curve_roots({p - edge, a1, a2, a3}, 3);
                        cuts.insert(cuts.end(), roots.begin(), roots.end());
                    }
            };
            axis(a.x, b.x, c.x, d.x, width, false);
            axis(a.y, b.y, c.y, d.y, height, true);
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end(),
                                   [](double x, double y) { return std::abs(x - y) < 1e-8; }),
                       cuts.end());
            double previous = 0;
            for (double t : cuts) {
                const double u = (t - previous) / (1 - previous);
                Point ab = a + (b - a) * u, bc = b + (c - b) * u, cd = c + (d - c) * u;
                Point abc = ab + (bc - ab) * u, bcd = bc + (cd - bc) * u,
                      mid = abc + (bcd - abc) * u;
                curve(a, ab, abc, mid, 1);
                a = mid;
                b = bcd;
                c = cd;
                previous = t;
            }
        }
        auto sample = [&](double t) {
            const double u = 1 - t;
            return a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + d * (t * t * t);
        };
        const Point e1 = sample(1.0 / 3) - b, e2 = sample(2.0 / 3) - c;
        auto deviation = [](Point p) {
            return std::max(std::abs(p.x), std::abs(p.y)) +
                   .5 * std::min(std::abs(p.x), std::abs(p.y));
        };
        const double error = deviation(
            {std::max(std::abs(e1.x), std::abs(e2.x)), std::max(std::abs(e1.y), std::abs(e2.y))});
        unsigned steps = 2;
        while (steps < 64 && error >= .0625 * (steps * steps - 1))
            steps *= 2;
        for (unsigned i = 1; i < steps; ++i) {
            auto p = sample(static_cast<double>(i) / steps);
            p.x = std::floor(p.x * 256) / 256;
            p.y = std::floor(p.y * 256) / 256;
            line(p);
        }
        line(d);
    }
};

Point point(const FT_Vector* p) {
    return {static_cast<double>(p->x) / 256, static_cast<double>(p->y) / 256};
}

int move_to(const FT_Vector* p, void* user) {
    auto& d = *static_cast<Decompose*>(user);
    try {
        d.line(d.start);
        d.current = d.start = point(p);
        return 0;
    } catch (...) {
        d.failed = true;
        return 1;
    }
}

int line_to(const FT_Vector* p, void* user) {
    auto& d = *static_cast<Decompose*>(user);
    try {
        d.line(point(p));
        return 0;
    } catch (...) {
        d.failed = true;
        return 1;
    }
}

int conic_to(const FT_Vector* c, const FT_Vector* p, void* user) {
    auto& d = *static_cast<Decompose*>(user);
    try {
        const auto end = point(p), control = point(c);
        d.quadratic(d.current, control, end);
        return 0;
    } catch (...) {
        d.failed = true;
        return 1;
    }
}

int cubic_to(const FT_Vector* a, const FT_Vector* b, const FT_Vector* p, void* user) {
    auto& d = *static_cast<Decompose*>(user);
    try {
        d.curve(d.current, point(a), point(b), point(p));
        return 0;
    } catch (...) {
        d.failed = true;
        return 1;
    }
}

void render(FT_Library, Canvas& canvas, FT_Outline& outline, const Paint& paint, double opacity) {
    if (outline.n_points == 0)
        return;
    FT_BBox bbox{};
    FT_Outline_Get_CBox(&outline, &bbox);
    const auto box_left =
        std::clamp(static_cast<int>(std::floor(static_cast<double>(bbox.xMin) / 256)), 0,
                   static_cast<int>(canvas.image.width));
    const auto right = std::clamp(static_cast<int>(std::ceil(static_cast<double>(bbox.xMax) / 256)),
                                  0, static_cast<int>(canvas.image.width));
    const auto top = std::clamp(static_cast<int>(std::floor(static_cast<double>(bbox.yMin) / 256)),
                                0, static_cast<int>(canvas.image.height));
    const auto bottom =
        std::clamp(static_cast<int>(std::ceil(static_cast<double>(bbox.yMax) / 256)), 0,
                   static_cast<int>(canvas.image.height));
    if (right <= box_left || bottom <= top)
        return;
    canvas.work += static_cast<std::size_t>(right - box_left) * static_cast<unsigned>(bottom - top);
    require(canvas.work <= 64U * 1024U * 1024U);
    Decompose d;
    d.width = canvas.image.width;
    d.height = canvas.image.height;
    d.clip = bbox.xMin < 0 || bbox.yMin < 0 ||
             bbox.xMax > static_cast<FT_Pos>(canvas.image.width) * 256 ||
             bbox.yMax > static_cast<FT_Pos>(canvas.image.height) * 256;
    const FT_Outline_Funcs funcs{move_to, line_to, conic_to, cubic_to, 0, 0};
    require(FT_Outline_Decompose(&outline, &funcs, &d) == 0 && !d.failed);
    d.line(d.start);
    const int width = static_cast<int>(canvas.image.width),
              height = static_cast<int>(canvas.image.height);

    struct ScanEdge {
        double x, step;
        int first, end, winding;
    };

    std::vector<ScanEdge> edges;
    edges.reserve(d.edges.size());
    for (const auto& edge : d.edges) {
        const int first = std::clamp(
            static_cast<int>(std::floor(std::min(edge.y0, edge.y1) * 4 + .5)), 0, height * 4);
        const int end = std::clamp(
            static_cast<int>(std::floor(std::max(edge.y0, edge.y1) * 4 + .5)), 0, height * 4);
        canvas.work += static_cast<std::size_t>(end - first);
        require(canvas.work <= 64U * 1024U * 1024U);
        if (end > first) {
            const bool downward = edge.y1 > edge.y0;
            const double x0 = downward ? edge.x0 : edge.x1;
            const double y0 = downward ? edge.y0 : edge.y1;
            const double slope =
                std::trunc((edge.x1 - edge.x0) / (edge.y1 - edge.y0) * 65536) / 65536;
            const double x = x0 + std::floor(((first + .5) / 4 - y0) * slope * 256) / 256;
            edges.push_back({x, slope / 4, first, end, downward ? 1 : -1});
        }
    }
    std::vector<Edge>{}.swap(d.edges);
    std::sort(edges.begin(), edges.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const ScanEdge*> active;
    std::vector<std::pair<double, int>> row;
    std::size_t next_edge = 0;
    std::vector<unsigned> coverage(static_cast<std::size_t>(width));
    const bool evenodd = (outline.flags & FT_OUTLINE_EVEN_ODD_FILL) != 0;
    for (int y = top; y < bottom; ++y) {
        std::fill(coverage.begin(), coverage.end(), 0);
        for (int sub = 0; sub < 4; ++sub) {
            const int scan = y * 4 + sub;
            std::erase_if(active, [scan](const auto* edge) { return edge->end <= scan; });
            while (next_edge < edges.size() && edges[next_edge].first <= scan)
                active.push_back(&edges[next_edge++]);
            row.clear();
            for (const auto* item : active) {
                const double x = item->x + (scan - item->first) * item->step;
                row.emplace_back(x, item->winding);
            }
            std::sort(row.begin(), row.end());
            int winding = 0;
            double start = 0;
            for (const auto& [x, dir] : row) {
                const bool was = evenodd ? (winding % 2 != 0) : (winding != 0);
                winding += dir;
                const bool now = evenodd ? (winding % 2 != 0) : (winding != 0);
                if (!was && now)
                    start = x;
                else if (was && !now) {
                    int left =
                        std::clamp(static_cast<int>(std::floor(start * 4 + .5)), 0, width * 4);
                    const int end =
                        std::clamp(static_cast<int>(std::floor(x * 4 + .5)), 0, width * 4);
                    while (left < end) {
                        const int next = std::min(end, (left / 4 + 1) * 4);
                        coverage[static_cast<std::size_t>(left / 4)] +=
                            static_cast<unsigned>(next - left);
                        left = next;
                    }
                }
            }
        }
        int x = 0;
        while (x < width) {
            int end = x + 1;
            while (end < width &&
                   coverage[static_cast<std::size_t>(end)] == coverage[static_cast<std::size_t>(x)])
                ++end;
            const unsigned alpha = std::min(255U, coverage[static_cast<std::size_t>(x)] * 16);
            if (alpha)
                composite(canvas, x, y, end - x, alpha, paint, opacity);
            x = end;
        }
    }
    require(!canvas.exhausted);
}

// Flatten only for dash lengths. FreeType expands all stroke widths from curves.
void flatten(std::vector<Point>& out, Point a, Point b, Point c, Point d, double tolerance,
             std::size_t& work, int depth = 0) {
    require(++work <= kMaxSegments);
    Point chord = d - a;
    const double len = std::hypot(chord.x, chord.y);
    const double error =
        len > 1e-20 ? std::max(std::abs(chord.x * (b.y - a.y) - chord.y * (b.x - a.x)),
                               std::abs(chord.x * (c.y - a.y) - chord.y * (c.x - a.x))) /
                          len
                    : std::max(std::hypot(b.x - a.x, b.y - a.y), std::hypot(c.x - a.x, c.y - a.y));
    if (error <= tolerance || depth >= 20) {
        out.push_back(d);
        return;
    }
    Point ab = (a + b) * .5, bc = (b + c) * .5, cd = (c + d) * .5, abc = (ab + bc) * .5,
          bcd = (bc + cd) * .5, mid = (abc + bcd) * .5;
    flatten(out, a, ab, abc, mid, tolerance, work, depth + 1);
    flatten(out, mid, bcd, cd, d, tolerance, work, depth + 1);
}

Path dashed(const Path& path, std::string_view pattern, double offset, double extent, double scale,
            std::size_t& canvas_work) {
    // Inherited arrays can be reparsed by many small paths, even if they
    // contain only whitespace and generate no dash fragments.
    canvas_work += pattern.size();
    require(canvas_work <= 64U * 1024U * 1024U);
    std::size_t work = 0;
    auto finish = [&]() {
        canvas_work += work;
        require(canvas_work <= 64U * 1024U * 1024U);
    };
    Numbers input{pattern};
    std::vector<double> lengths;
    while (input.more()) {
        require(++work <= kMaxSegments);
        const auto start = input.at;
        input.get();
        while (
            input.at < pattern.size() &&
            ((pattern[input.at] >= 'a' && pattern[input.at] <= 'z') || pattern[input.at] == '%')) {
            ++input.at;
        }
        const double value = length(pattern.substr(start, input.at - start), extent);
        require(value >= 0);
        lengths.push_back(value);
    }
    if (lengths.empty()) {
        finish();
        return path;
    }
    if (lengths.size() % 2 != 0) {
        const auto copy = lengths;
        lengths.insert(lengths.end(), copy.begin(), copy.end());
    }
    double total = 0;
    for (double value : lengths) {
        total += value;
    }
    require(std::isfinite(total));
    if (total <= 0) {
        finish();
        return path;
    }
    Path result;
    Point current{}, start{};
    std::size_t dash = 0;
    double remaining = 0;
    bool open = false;
    auto append = [&](char kind, Point p) {
        require(++work <= kMaxSegments);
        result.segments.push_back({kind, p, {}, {}});
    };
    auto advance = [&]() {
        for (std::size_t i = 0; i <= lengths.size(); ++i) {
            require(++work <= kMaxSegments);
            dash = (dash + 1) % lengths.size();
            remaining = lengths[dash];
            if (remaining > 0) {
                return;
            }
        }
        throw Failure{};
    };
    auto reset = [&]() {
        dash = 0;
        double phase = std::fmod(offset, total);
        if (phase < 0) {
            phase += total;
        }
        remaining = lengths[0];
        while (phase >= remaining) {
            phase -= remaining;
            advance();
        }
        remaining -= phase;
        open = false;
    };
    auto line = [&](Point end) {
        double distance = std::hypot(end.x - current.x, end.y - current.y);
        require(std::isfinite(distance));
        if (distance <= 1e-15) {
            return;
        }
        Point unit = (end - current) * (1 / distance);
        while (distance > 1e-12) {
            require(++work <= kMaxSegments);
            if (remaining <= 1e-12) {
                advance();
                open = false;
            }
            const double step = std::min(distance, remaining);
            Point next = step >= distance ? end : current + unit * step;
            if (dash % 2 == 0) {
                if (!open) {
                    append('M', current);
                }
                append('L', next);
                open = true;
            } else {
                open = false;
            }
            current = next;
            distance -= step;
            remaining -= step;
        }
        current = end;
    };
    std::size_t subpath_begin = 0;
    for (const auto& segment : path.segments) {
        require(++work <= kMaxSegments);
        if (segment.kind == 'M') {
            subpath_begin = result.segments.size();
            current = start = segment.p;
            reset();
        } else if (segment.kind == 'L') {
            line(segment.p);
        } else if (segment.kind == 'C') {
            std::vector<Point> poly;
            flatten(poly, current, segment.c1, segment.c2, segment.p, .03 / std::max(scale, 1e-9),
                    work);
            for (auto p : poly) {
                line(p);
            }
        } else if (segment.kind == 'Z') {
            line(start);
            if (result.segments.size() > subpath_begin + 1 &&
                result.segments[subpath_begin].p.x == start.x &&
                result.segments[subpath_begin].p.y == start.y &&
                result.segments.back().p.x == start.x && result.segments.back().p.y == start.y) {
                std::size_t last = result.segments.size() - 1;
                while (last > subpath_begin && result.segments[last].kind != 'M') {
                    --last;
                }
                if (last == subpath_begin) {
                    append('Z', {});
                } else {
                    std::size_t first_end = subpath_begin + 1;
                    while (first_end < last && result.segments[first_end].kind != 'M') {
                        ++first_end;
                    }
                    std::vector<Segment> joined(
                        result.segments.begin() + static_cast<std::ptrdiff_t>(last),
                        result.segments.end());
                    joined.insert(
                        joined.end(),
                        result.segments.begin() + static_cast<std::ptrdiff_t>(subpath_begin + 1),
                        result.segments.begin() + static_cast<std::ptrdiff_t>(first_end));
                    joined.insert(joined.end(),
                                  result.segments.begin() + static_cast<std::ptrdiff_t>(first_end),
                                  result.segments.begin() + static_cast<std::ptrdiff_t>(last));
                    result.segments.resize(subpath_begin);
                    result.segments.insert(result.segments.end(), joined.begin(), joined.end());
                }
            }
        }
    }
    finish();
    return result;
}
}  // namespace

Raster::Raster() {
    require(FT_Init_FreeType(&library) == 0);
    if (FT_Stroker_New(library, &stroker) != 0) {
        FT_Done_FreeType(library);
        library = nullptr;
        throw Failure{};
    }
}

Raster::~Raster() {
    FT_Stroker_Done(stroker);
    FT_Done_FreeType(library);
}

void Raster::draw(Canvas& canvas, const Path& source, const Matrix& world, const Paint& paint,
                  double opacity, bool evenodd, const Style* stroke, double width, double height) {
    const Matrix matrix =
        Matrix{1, 0, 0, 1, -static_cast<double>(canvas.x), -static_cast<double>(canvas.y)} * world;
    if (source.segments.empty() || opacity <= 0 ||
        (!paint.texture && paint.stops.empty() && paint.solid[3] == 0)) {
        return;
    }
    canvas.work += source.segments.size();
    require(canvas.work <= 64U * 1024U * 1024U);
    if (!stroke) {
        Outline result;
        for (const auto& s : source.segments) {
            if (s.kind == 'M') {
                result.close();
                result.point(matrix.apply(s.p), FT_CURVE_TAG_ON, true);
            } else if (s.kind == 'L') {
                result.point(matrix.apply(s.p), FT_CURVE_TAG_ON, true);
            } else if (s.kind == 'C') {
                result.point(matrix.apply(s.c1), FT_CURVE_TAG_CUBIC, true);
                result.point(matrix.apply(s.c2), FT_CURVE_TAG_CUBIC, true);
                result.point(matrix.apply(s.p), FT_CURVE_TAG_ON, true);
            } else if (s.kind == 'Z') {
                result.close();
            }
        }
        result.close();
        auto outline = result.get(evenodd);
        render(library, canvas, outline, paint, opacity);
        return;
    }
    const double extent = std::hypot(width, height) / std::sqrt(2.0),
                 line_width = length(stroke->stroke_width, extent, 1);
    require(line_width >= 0);
    if (line_width == 0) {
        return;
    }
    const double scale = std::max(std::hypot(matrix.a, matrix.b), std::hypot(matrix.c, matrix.d));
    if (scale <= 1e-15) {
        return;
    }
    require(std::isfinite(scale));
    const auto box = bounds(source);
    // Anchor the local coordinates to whole scaled units so unused moves do
    // not shift FreeType's fixed-point rounding phase.
    const Point origin{std::floor(box.x * scale) / scale, std::floor(box.y * scale) / scale};
    const Path dash = stroke->dasharray != "none"
                          ? dashed(source, stroke->dasharray, length(stroke->dashoffset, extent),
                                   extent, scale, canvas.work)
                          : Path{};
    const Path& path = stroke->dasharray != "none" ? dash : source;
    auto convert = [&](Point p) { return vector((p - origin) * scale); };
    require(stroke->miterlimit <= 32767);
    const auto cap = stroke->linecap == "round"    ? FT_STROKER_LINECAP_ROUND
                     : stroke->linecap == "square" ? FT_STROKER_LINECAP_SQUARE
                                                   : FT_STROKER_LINECAP_BUTT;
    const auto join = stroke->linejoin == "round"   ? FT_STROKER_LINEJOIN_ROUND
                      : stroke->linejoin == "bevel" ? FT_STROKER_LINEJOIN_BEVEL
                                                    : FT_STROKER_LINEJOIN_MITER_FIXED;
    const auto radius = fixed(line_width * scale / 2);
    if (radius == 0)
        return;
    FT_Stroker_Set(stroker, radius, cap, join,
                   static_cast<FT_Fixed>(std::lround(stroke->miterlimit * 65536)));
    Outline result;
    for (std::size_t i = 0; i < path.segments.size();) {
        require(path.segments[i].kind == 'M');
        std::size_t end = i + 1;
        while (end < path.segments.size() && path.segments[end].kind != 'M')
            ++end;
        // A move alone has no stroke geometry, including when caps are round.
        if (end == i + 1) {
            i = end;
            continue;
        }
        bool has_edge = false, closed = false;
        FT_Vector previous = convert(path.segments[i].p);
        Outline input;
        auto point = [&](Point p, Tag tag) { input.point((p - origin) * scale, tag); };
        point(path.segments[i].p, FT_CURVE_TAG_ON);
        for (std::size_t j = i + 1; j < end; ++j) {
            const auto& part = path.segments[j];
            if (part.kind == 'Z') {
                closed = true;
                continue;
            }
            const auto p = convert(part.p);
            has_edge = has_edge || p.x != previous.x || p.y != previous.y;
            if (part.kind == 'C') {
                const auto c1 = convert(part.c1), c2 = convert(part.c2);
                has_edge = has_edge || c1.x != previous.x || c1.y != previous.y ||
                           c2.x != previous.x || c2.y != previous.y;
                point(part.c1, FT_CURVE_TAG_CUBIC);
                point(part.c2, FT_CURVE_TAG_CUBIC);
            }
            point(part.p, FT_CURVE_TAG_ON);
            previous = p;
        }
        if (!has_edge && (stroke->linecap == "round" || stroke->linecap == "square")) {
            input.points.erase(input.points.begin() + 1, input.points.end());
            input.tags.erase(input.tags.begin() + 1, input.tags.end());
            auto p = input.points.front();
            ++p.x;
            input.points.push_back(p);
            input.tags.push_back(FT_CURVE_TAG_ON);
            closed = false;
        }
        input.close();
        auto source_outline = input.get(false);
        // ParseOutline checks whether curves actually generated an edge before ending a
        // subpath. Direct Begin/End calls cannot observe the stroker's internal tolerance.
        require(FT_Stroker_ParseOutline(stroker, &source_outline, !closed) == 0);
        FT_UInt points{}, contours{};
        require(FT_Stroker_GetCounts(stroker, &points, &contours) == 0 &&
                result.points.size() + points <= 32767 &&
                result.contours.size() + contours <= 32767);
        if (points != 0) {
            Outline stroked;
            stroked.points.resize(points);
            stroked.tags.resize(points);
            stroked.contours.resize(contours);
            auto out = stroked.get(false);
            out.n_points = out.n_contours = 0;
            FT_Stroker_Export(stroker, &out);
            const auto offset = result.points.size();
            for (auto p : stroked.points) {
                Point local{static_cast<double>(p.x) / (64 * scale) + origin.x,
                            static_cast<double>(p.y) / (64 * scale) + origin.y};
                result.points.push_back(high_vector(matrix.apply(local)));
            }
            result.tags.insert(result.tags.end(), stroked.tags.begin(), stroked.tags.end());
            for (auto c : stroked.contours)
                result.contours.push_back(static_cast<Contour>(offset + static_cast<unsigned>(c)));
        }
        i = end;
    }
    auto outline = result.get(false);
    render(library, canvas, outline, paint, opacity);
}
}  // namespace rummage::svg
