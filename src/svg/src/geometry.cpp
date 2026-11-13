#include <numbers>

#include "internal.hpp"

namespace rummage::svg {
namespace {
constexpr double kPi = std::numbers::pi;

class Builder {
public:
    Path path;
    Point current{}, start{};

    explicit Builder(std::size_t& count) : count_(count) {}

    void add(char kind, Point p = {}, Point c1 = {}, Point c2 = {}) {
        require(++count_ <= kMaxSegments);
        for (auto point : {p, c1, c2}) {
            require(std::isfinite(point.x) && std::isfinite(point.y) && std::abs(point.x) <= 1e12 &&
                    std::abs(point.y) <= 1e12);
        }
        path.segments.push_back({kind, p, c1, c2});
        if (kind == 'M') {
            start = p;
        }
        current = kind == 'Z' ? start : p;
    }

    void arc(double rx, double ry, double rotation, bool large, bool sweep, Point end) {
        rx = std::abs(rx);
        ry = std::abs(ry);
        require(rx <= 1e12 && ry <= 1e12);
        if (current.x == end.x && current.y == end.y) {
            return;
        }
        if (rx == 0 || ry == 0) {
            add('L', end);
            return;
        }
        const double phi = std::remainder(rotation, 360.0) * kPi / 180, cs = std::cos(phi),
                     sn = std::sin(phi);
        const Point delta = (current - end) * .5;
        const double xp = cs * delta.x + sn * delta.y, yp = -sn * delta.x + cs * delta.y;
        const double ratio = std::hypot(xp / rx, yp / ry);
        require(std::isfinite(ratio));
        if (ratio > 1) {
            rx *= ratio;
            ry *= ratio;
        }
        const double ux = xp / rx, uy = yp / ry, den = ux * ux + uy * uy;
        if (den == 0) {
            add('L', end);
            return;
        }
        const double factor = (large == sweep ? -1 : 1) * std::sqrt(std::max(0.0, (1 - den) / den));
        const double cxp = factor * rx * uy, cyp = -factor * ry * ux;
        const Point centre{cs * cxp - sn * cyp + (current.x + end.x) * .5,
                           sn * cxp + cs * cyp + (current.y + end.y) * .5};
        const double begin = std::atan2((yp - cyp) / ry, (xp - cxp) / rx);
        double angle = std::atan2((-yp - cyp) / ry, (-xp - cxp) / rx) - begin;
        if (sweep && angle < 0) {
            angle += 2 * kPi;
        }
        if (!sweep && angle > 0) {
            angle -= 2 * kPi;
        }
        // Radius correction or a subnormal denominator can overflow the
        // centre calculation even with finite input. Reject before converting
        // the subdivision count to an integer.
        require(std::isfinite(begin) && std::isfinite(angle));
        const int pieces = std::max(1, static_cast<int>(std::ceil(std::abs(angle) / (kPi / 2))));
        const double step = angle / pieces;
        auto point = [&](double t) {
            return Point{centre.x + cs * rx * std::cos(t) - sn * ry * std::sin(t),
                         centre.y + sn * rx * std::cos(t) + cs * ry * std::sin(t)};
        };
        auto tangent = [&](double t) {
            return Point{-cs * rx * std::sin(t) - sn * ry * std::cos(t),
                         -sn * rx * std::sin(t) + cs * ry * std::cos(t)};
        };
        for (int i = 0; i < pieces; ++i) {
            double a = begin + i * step, b = a + step, k = 4.0 / 3 * std::tan(step / 4);
            Point to = i + 1 == pieces ? end : point(b);
            add('C', to, point(a) + tangent(a) * k, point(b) - tangent(b) * k);
        }
    }

private:
    std::size_t& count_;
};

void data(Builder& b, std::string_view text) {
    Numbers input{text};
    char command = 0, previous = 0;
    Point control{};
    while (input.more()) {
        char c = text[input.at];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            command = c;
            ++input.at;
        }
        require(command != 0);
        const bool relative = command >= 'a' && command <= 'z';
        const char upper = relative ? static_cast<char>(command - 'a' + 'A') : command;
        require(!b.path.segments.empty() || upper == 'M');
        const Point origin = b.current;
        auto point = [&]() {
            Point p{input.get(), input.get()};
            return relative ? p + origin : p;
        };
        if (upper == 'Z') {
            b.add('Z');
            command = 0;
            previous = 'Z';
            continue;
        }
        require(input.more());
        if (previous == 'Z' && upper != 'M') {
            b.add('M', b.current);
        }
        if (upper == 'M') {
            b.add('M', point());
            command = relative ? 'l' : 'L';
        } else if (upper == 'L') {
            b.add('L', point());
        } else if (upper == 'H') {
            double x = input.get();
            b.add('L', {relative ? origin.x + x : x, origin.y});
        } else if (upper == 'V') {
            double y = input.get();
            b.add('L', {origin.x, relative ? origin.y + y : y});
        } else if (upper == 'C') {
            auto c1 = point(), c2 = point(), to = point();
            b.add('C', to, c1, c2);
            control = c2;
        } else if (upper == 'S') {
            auto c1 = (previous == 'C' || previous == 'S') ? origin * 2 - control : origin;
            auto c2 = point(), to = point();
            b.add('C', to, c1, c2);
            control = c2;
        } else if (upper == 'Q' || upper == 'T') {
            Point q = upper == 'Q'                           ? point()
                      : (previous == 'Q' || previous == 'T') ? origin * 2 - control
                                                             : origin;
            auto to = point();
            b.add('C', to, origin + (q - origin) * (2.0 / 3), to + (q - to) * (2.0 / 3));
            control = q;
        } else if (upper == 'A') {
            double rx = input.get(), ry = input.get(), rotation = input.get();
            bool large = input.flag(), sweep = input.flag();
            b.arc(rx, ry, rotation, large, sweep, point());
        } else {
            throw Failure{};
        }
        previous = upper;
    }
}
}  // namespace

Path geometry(const Node& node, double width, double height, std::size_t& count) {
    Builder b{count};
    auto x = [&](std::string_view key, double fallback = 0) {
        return length(node.attr(key), width, fallback);
    };
    auto y = [&](std::string_view key, double fallback = 0) {
        return length(node.attr(key), height, fallback);
    };
    if (node.name == "path") {
        data(b, node.attr("d"));
    } else if (node.name == "rect") {
        double left = x("x"), top = y("y"), w = x("width"), h = y("height");
        require(w >= 0 && h >= 0);
        if (w == 0 || h == 0) {
            return {};
        }
        double rx = x("rx", y("ry")), ry = y("ry", rx);
        require(rx >= 0 && ry >= 0);
        rx = std::min(rx, w / 2);
        ry = std::min(ry, h / 2);
        b.add('M', {left + rx, top});
        b.add('L', {left + w - rx, top});
        if (rx > 0 && ry > 0) {
            b.arc(rx, ry, 0, false, true, {left + w, top + ry});
        } else {
            b.add('L', {left + w, top});
        }
        b.add('L', {left + w, top + h - ry});
        if (rx > 0 && ry > 0) {
            b.arc(rx, ry, 0, false, true, {left + w - rx, top + h});
        } else {
            b.add('L', {left + w, top + h});
        }
        b.add('L', {left + rx, top + h});
        if (rx > 0 && ry > 0) {
            b.arc(rx, ry, 0, false, true, {left, top + h - ry});
        } else {
            b.add('L', {left, top + h});
        }
        b.add('L', {left, top + ry});
        if (rx > 0 && ry > 0) {
            b.arc(rx, ry, 0, false, true, {left + rx, top});
        } else {
            b.add('L', {left, top});
        }
        b.add('Z');
    } else if (node.name == "circle" || node.name == "ellipse") {
        double cx = x("cx"), cy = y("cy"),
               rx = node.name == "circle"
                        ? length(node.attr("r"), std::hypot(width, height) / std::sqrt(2.0))
                        : x("rx"),
               ry = node.name == "circle" ? rx : y("ry");
        require(rx >= 0 && ry >= 0);
        if (rx == 0 || ry == 0) {
            return {};
        }
        b.add('M', {cx + rx, cy});
        b.arc(rx, ry, 0, false, true, {cx, cy + ry});
        b.arc(rx, ry, 0, false, true, {cx - rx, cy});
        b.arc(rx, ry, 0, false, true, {cx, cy - ry});
        b.arc(rx, ry, 0, false, true, {cx + rx, cy});
        b.add('Z');
    } else if (node.name == "line") {
        b.add('M', {x("x1"), y("y1")});
        b.add('L', {x("x2"), y("y2")});
    } else if (node.name == "polygon" || node.name == "polyline") {
        Numbers input{node.attr("points")};
        bool first = true;
        while (input.more()) {
            b.add(first ? 'M' : 'L', {input.get(), input.get()});
            first = false;
        }
        if (!first && node.name == "polygon") {
            b.add('Z');
        }
    }
    return std::move(b.path);
}

Box bounds(const Path& path) {
    double minx = std::numeric_limits<double>::infinity(), miny = minx, maxx = -minx, maxy = -minx;
    Point current{}, start{};
    auto include = [&](Point p) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    };
    for (const auto& s : path.segments) {
        if (s.kind == 'M') {
            current = start = s.p;
            include(current);
        } else if (s.kind == 'L') {
            include(s.p);
            current = s.p;
        } else if (s.kind == 'Z') {
            include(start);
            current = start;
        } else if (s.kind == 'C') {
            include(s.p);
            auto at = [&](double t) {
                double u = 1 - t;
                include(current * (u * u * u) + s.c1 * (3 * u * u * t) + s.c2 * (3 * u * t * t) +
                        s.p * (t * t * t));
            };
            auto axis = [&](double p0, double p1, double p2, double p3) {
                double a = -p0 + 3 * p1 - 3 * p2 + p3, b = 2 * (p0 - 2 * p1 + p2), c = p1 - p0;
                if (std::abs(a) < 1e-15) {
                    if (std::abs(b) > 1e-15) {
                        double t = -c / b;
                        if (t > 0 && t < 1) {
                            at(t);
                        }
                    }
                    return;
                }
                double disc = b * b - 4 * a * c;
                if (disc < 0) {
                    return;
                }
                double root = std::sqrt(disc);
                for (double t : {(-b + root) / (2 * a), (-b - root) / (2 * a)}) {
                    if (t > 0 && t < 1) {
                        at(t);
                    }
                }
            };
            axis(current.x, s.c1.x, s.c2.x, s.p.x);
            axis(current.y, s.c1.y, s.c2.y, s.p.y);
            current = s.p;
        }
    }
    if (!std::isfinite(minx)) {
        return {};
    }
    return {minx, miny, maxx - minx, maxy - miny};
}
}  // namespace rummage::svg
