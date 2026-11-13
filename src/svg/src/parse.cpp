#include <charconv>
#include <numbers>

#include "internal.hpp"

namespace rummage::svg {
std::string_view trim(std::string_view s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == s.npos) {
        return {};
    }
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

void Numbers::skip() {
    while (at < input.size() && (input[at] == ' ' || input[at] == '\t' || input[at] == '\r' ||
                                 input[at] == '\n' || input[at] == ',')) {
        ++at;
    }
}

bool Numbers::more() {
    skip();
    return at < input.size();
}

double Numbers::get() {
    skip();
    require(at < input.size());
    if (input[at] == '+') {
        ++at;
    }
    require(at < input.size());
    double value{};
    auto result = std::from_chars(input.data() + at, input.data() + input.size(), value);
    require(result.ec == std::errc{} && result.ptr != input.data() + at && std::isfinite(value));
    at = static_cast<std::size_t>(result.ptr - input.data());
    return value;
}

bool Numbers::flag() {
    skip();
    require(at < input.size() && (input[at] == '0' || input[at] == '1'));
    return input[at++] == '1';
}

double number(std::string_view s, double fallback) {
    s = trim(s);
    if (s.empty()) {
        return fallback;
    }
    Numbers input{s};
    auto n = input.get();
    require(!input.more());
    return n;
}

std::vector<double> numbers(std::string_view s) {
    Numbers input{s};
    std::vector<double> result;
    while (input.more()) {
        require(result.size() < kMaxSegments);
        result.push_back(input.get());
    }
    return result;
}

double length(std::string_view s, double extent, double fallback) {
    s = trim(s);
    if (s.empty()) {
        return fallback;
    }
    Numbers input{s};
    double value = input.get();
    auto unit = trim(s.substr(input.at));
    if (unit == "%") {
        value *= extent / 100;
    } else if (unit == "pt") {
        value *= 96.0 / 72;
    } else if (unit == "pc") {
        value *= 16;
    } else if (unit == "mm") {
        value *= 96.0 / 25.4;
    } else if (unit == "cm") {
        value *= 96.0 / 2.54;
    } else if (unit == "in") {
        value *= 96;
    } else {
        require(unit.empty() || unit == "px");
    }
    require(std::isfinite(value));
    return value;
}

Matrix Matrix::operator*(const Matrix& m) const {
    Matrix out{a * m.a + c * m.b, b * m.a + d * m.b,     a * m.c + c * m.d,
               b * m.c + d * m.d, a * m.e + c * m.f + e, b * m.e + d * m.f + f};
    for (double v : {out.a, out.b, out.c, out.d, out.e, out.f}) {
        require(std::isfinite(v));
    }
    return out;
}

std::optional<Matrix> Matrix::inverse() const {
    double det = a * d - b * c;
    require(std::isfinite(det));
    if (std::abs(det) < 1e-30) {
        return {};
    }
    Matrix out{d / det, -b / det, -c / det, a / det, (c * f - d * e) / det, (b * e - a * f) / det};
    for (double v : {out.a, out.b, out.c, out.d, out.e, out.f}) {
        if (!std::isfinite(v)) {
            return {};
        }
    }
    return out;
}

Matrix transform(std::string_view text) {
    Matrix out;
    while (!(text = trim(text)).empty()) {
        if (text.starts_with(',')) {
            text.remove_prefix(1);
            text = trim(text);
        }
        auto open = text.find('('), close = text.find(')');
        require(open != text.npos && close != text.npos && close > open);
        auto name = trim(text.substr(0, open));
        auto args = numbers(text.substr(open + 1, close - open - 1));
        Matrix m;
        if (name == "matrix") {
            require(args.size() == 6);
            m = {args[0], args[1], args[2], args[3], args[4], args[5]};
        } else if (name == "translate") {
            require(args.size() == 1 || args.size() == 2);
            m.e = args[0];
            m.f = args.size() == 2 ? args[1] : 0;
        } else if (name == "scale") {
            require(args.size() == 1 || args.size() == 2);
            m.a = args[0];
            m.d = args.size() == 2 ? args[1] : args[0];
        } else if (name == "rotate") {
            require(args.size() == 1 || args.size() == 3);
            double angle = std::remainder(args[0], 360.0) * std::numbers::pi / 180;
            m = {std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0, 0};
            if (args.size() == 3) {
                m = Matrix{1, 0, 0, 1, args[1], args[2]} * m *
                    Matrix{1, 0, 0, 1, -args[1], -args[2]};
            }
        } else if (name == "skewX" || name == "skewY") {
            require(args.size() == 1);
            double t = std::tan(std::remainder(args[0], 180.0) * std::numbers::pi / 180);
            require(std::isfinite(t));
            if (name == "skewX") {
                m.c = t;
            } else {
                m.b = t;
            }
        } else {
            throw Failure{};
        }
        out = out * m;
        text.remove_prefix(close + 1);
    }
    return out;
}

Matrix viewport(std::string_view viewbox, std::string_view aspect, double width, double height) {
    if (trim(viewbox).empty()) {
        return {};
    }
    auto box = numbers(viewbox);
    require(box.size() == 4 && box[2] > 0 && box[3] > 0);
    double sx = width / box[2], sy = height / box[3];
    double tx = -box[0] * sx, ty = -box[1] * sy;
    aspect = trim(aspect);
    if (aspect.starts_with("defer ")) {
        aspect = trim(aspect.substr(6));
    }
    if (aspect != "none") {
        if (aspect.empty()) {
            aspect = "xMidYMid meet";
        }
        auto end = aspect.find_first_of(" \t\r\n");
        auto align = aspect.substr(0, end);
        auto mode = end == aspect.npos ? std::string_view{"meet"} : trim(aspect.substr(end));
        require(
            align.size() == 8 &&
            (align.substr(0, 4) == "xMin" || align.substr(0, 4) == "xMid" ||
             align.substr(0, 4) == "xMax") &&
            (align.substr(4) == "YMin" || align.substr(4) == "YMid" || align.substr(4) == "YMax") &&
            (mode == "meet" || mode == "slice"));
        sx = sy = mode == "slice" ? std::max(sx, sy) : std::min(sx, sy);
        double ax = align.substr(0, 4) == "xMin" ? 0 : align.substr(0, 4) == "xMid" ? .5 : 1;
        double ay = align.substr(4) == "YMin" ? 0 : align.substr(4) == "YMid" ? .5 : 1;
        tx = -box[0] * sx + (width - box[2] * sx) * ax;
        ty = -box[1] * sy + (height - box[3] * sy) * ay;
    }
    return {sx, 0, 0, sy, tx, ty};
}
}  // namespace rummage::svg
