#include "opcodes.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>

namespace rummage::formats::image::dng {
namespace {

constexpr std::uint32_t kWarpRectilinear = 1;
constexpr std::uint32_t kFixBadPixelsConstant = 4;
constexpr std::uint32_t kFixBadPixelsList = 5;
constexpr std::uint32_t kGainMap = 9;
constexpr std::uint32_t kOptional = 1;
constexpr std::uint32_t kSkipForPreview = 2;
constexpr std::size_t kMaxListBytes = std::size_t{8} << 20;
constexpr std::uint64_t kMaxPoints = std::uint64_t{1} << 20;
constexpr float kMaxGain = 16.0F;
constexpr std::uint32_t kMaxWarpPlanes = 4;
constexpr std::size_t kWarpPlaneBytes = 48;   // six doubles
constexpr std::size_t kWarpCentreBytes = 16;  // two doubles

// The opcodes the specification names (DNG 1.7), for the messages.
std::string name_of(std::uint32_t id) {
    static constexpr std::array<const char*, 14> kNames{
        "WarpRectilinear",  "WarpFisheye",     "FixVignetteRadial", "FixBadPixelsConstant",
        "FixBadPixelsList", "TrimBounds",      "MapTable",          "MapPolynomial",
        "GainMap",          "DeltaPerRow",     "DeltaPerColumn",    "ScalePerRow",
        "ScalePerColumn",   "WarpRectilinear2"};
    if (id >= 1 && id <= kNames.size()) {
        return kNames.at(id - 1);
    }
    return "opcode " + std::to_string(id);
}

// Big-endian values read one after the other; any read past the end makes ok false (and gives 0).
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}

    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (const std::uint8_t b : take(4)) {
            v = (v << 8) | b;
        }
        return v;
    }

    double f64() {
        std::uint64_t v = 0;
        for (const std::uint8_t b : take(8)) {
            v = (v << 8) | b;
        }
        return std::bit_cast<double>(v);
    }

    float f32() { return std::bit_cast<float>(u32()); }

    std::span<const std::uint8_t> take(std::size_t length) {
        if (!ok || length > bytes_.size() - at_) {
            ok = false;
            return {};
        }
        const std::span<const std::uint8_t> part = bytes_.subspan(at_, length);
        at_ += length;
        return part;
    }

    [[nodiscard]] std::size_t left() const { return bytes_.size() - at_; }

    bool ok{true};

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_{0};
};

bool fail(std::string& error, std::string why) {
    error = std::move(why);
    return false;
}

// A GainMap's parameters; nullopt when damaged. An empty gains means the map changes nothing.
std::optional<GainMap> gain_map(std::span<const std::uint8_t> body) {
    Reader r{body};
    GainMap map;
    for (std::uint32_t& side : map.area) {
        side = r.u32();
    }
    map.plane = r.u32();
    map.planes = r.u32();
    map.row_pitch = r.u32();
    map.column_pitch = r.u32();
    map.points_v = r.u32();
    map.points_h = r.u32();
    map.spacing_v = r.f64();
    map.spacing_h = r.f64();
    map.origin_v = r.f64();
    map.origin_h = r.f64();
    map.map_planes = r.u32();
    if (!r.ok || map.area[0] >= map.area[2] || map.area[1] >= map.area[3] || map.planes == 0 ||
        map.row_pitch == 0 || map.column_pitch == 0 || map.points_v == 0 || map.points_h == 0 ||
        map.map_planes == 0 || !std::isfinite(map.origin_v) || !std::isfinite(map.origin_h) ||
        (map.points_v > 1 && (!std::isfinite(map.spacing_v) || map.spacing_v <= 0.0)) ||
        (map.points_h > 1 && (!std::isfinite(map.spacing_h) || map.spacing_h <= 0.0))) {
        return std::nullopt;
    }
    const std::uint64_t points = std::uint64_t{map.points_v} * map.points_h;  // under 2^64
    if (points > kMaxPoints || map.map_planes > kMaxPoints ||
        points * map.map_planes > kMaxPoints || points * map.map_planes * 4 > r.left()) {
        return std::nullopt;
    }
    map.gains.resize(points * map.map_planes);
    bool identity = true;
    for (float& gain : map.gains) {
        gain = r.f32();
        if (std::isnan(gain) || gain < 0.0F || gain > kMaxGain) {
            return std::nullopt;
        }
        identity = identity && gain == 1.0F;
    }
    if (identity) {
        map.gains.clear();
    }
    return map;
}

std::optional<Warp> warp(std::span<const std::uint8_t> body) {
    Reader r{body};
    const std::uint32_t planes = r.u32();
    if (!r.ok || planes == 0 || planes > kMaxWarpPlanes ||
        r.left() < (std::size_t{planes} * kWarpPlaneBytes) + kWarpCentreBytes) {
        return std::nullopt;
    }
    const std::uint32_t chosen = planes == 3 ? 1 : 0;
    Warp w;
    for (std::uint32_t p = 0; p < planes; ++p) {
        std::array<double, 6> k{};
        for (double& v : k) {
            v = r.f64();
        }
        if (p == chosen) {
            w.radial = {k[0], k[1], k[2], k[3]};
            w.tangential = {k[4], k[5]};
        }
    }
    w.centre_x = r.f64();
    w.centre_y = r.f64();
    const auto finite = [](double v) { return std::isfinite(v); };
    if (!r.ok || !std::ranges::all_of(w.radial, finite) ||
        !std::ranges::all_of(w.tangential, finite) || !finite(w.centre_x) || !finite(w.centre_y)) {
        return std::nullopt;
    }
    return w;
}

}  // namespace

namespace {

GainMap::Step step_of(double relative, double origin, double spacing, std::uint32_t points) {
    if (points == 1) {
        return {};
    }
    const double last = points - 1;
    const double position = std::clamp((relative - origin) / spacing, 0.0, last);
    const std::uint32_t first = std::min(static_cast<std::uint32_t>(position), points - 2);
    return {.first = first, .fraction = position - first};
}

}  // namespace

GainMap::Step GainMap::step_v(double v) const { return step_of(v, origin_v, spacing_v, points_v); }

GainMap::Step GainMap::step_h(double h) const { return step_of(h, origin_h, spacing_h, points_h); }

float GainMap::at(double v, double h, std::uint32_t p) const {
    const Step down = step_v(v);
    const Step across = step_h(h);
    const std::uint32_t next_row = std::min(down.first + 1, points_v - 1);
    const std::uint32_t next_column = std::min(across.first + 1, points_h - 1);
    const auto along = [&](std::uint32_t row) {
        const auto left = static_cast<double>(gain(row, across.first, p));
        return left + ((static_cast<double>(gain(row, next_column, p)) - left) * across.fraction);
    };
    const double upper = along(down.first);
    return static_cast<float>(upper + ((along(next_row) - upper) * down.fraction));
}

WarpMap::WarpMap(const Warp& warp, double width, double height)
    : warp_{warp},
      cx_{warp.centre_x * width},
      cy_{warp.centre_y * height},
      reach_{std::hypot(std::max(cx_, width - cx_), std::max(cy_, height - cy_))} {}

std::array<double, 2> WarpMap::source(double x, double y) const {
    // DNG specification, WarpRectilinear: distances from the optical centre in units of the
    // largest one to a corner, moved radially and tangentially.
    if (!(reach_ > 0.0)) {
        return {x, y};
    }
    const std::array<double, 4>& k = warp_.radial;
    const std::array<double, 2>& t = warp_.tangential;
    const double dx = (x - cx_) / reach_;
    const double dy = (y - cy_) / reach_;
    const double r2 = (dx * dx) + (dy * dy);
    const double f = k[0] + (r2 * (k[1] + (r2 * (k[2] + (r2 * k[3])))));
    const double tx = (t[0] * 2.0 * dx * dy) + (t[1] * (r2 + (2.0 * dx * dx)));
    const double ty = (t[1] * 2.0 * dx * dy) + (t[0] * (r2 + (2.0 * dy * dy)));
    return {cx_ + (reach_ * ((f * dx) + tx)), cy_ + (reach_ * ((f * dy) + ty))};
}

bool read_list(std::span<const std::uint8_t> bytes, int number, Opcodes& opcodes,
               std::string& error) {
    Reader r{bytes};
    const std::uint32_t count = r.u32();
    if (!r.ok || count > r.left() / 16) {
        return fail(error, "a DNG with a damaged opcode list");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t id = r.u32();
        static_cast<void>(r.u32());  // the DNG version it came with
        const std::uint32_t flags = r.u32();
        const std::uint32_t size = r.u32();
        const std::span<const std::uint8_t> body = r.take(size);
        if (!r.ok) {
            return fail(error, "a DNG with a damaged opcode list");
        }
        const bool may_skip = (flags & (kOptional | kSkipForPreview)) != 0;
        const std::string damaged = "a DNG with a damaged " + name_of(id);
        if (id == kFixBadPixelsConstant || id == kFixBadPixelsList) {
            continue;  // a site among the three or more it is averaged with
        }
        if (id == kGainMap && number >= 2) {
            std::optional<GainMap> map = gain_map(body);
            if (!map) {
                return fail(error, damaged);
            }
            if (map->gains.empty()) {
                continue;  // all ones
            }
            if (number == 3 && opcodes.warp) {
                if (may_skip) {
                    continue;
                }
                return fail(error, "a DNG with a GainMap after its WarpRectilinear");
            }
            (number == 2 ? opcodes.before_demosaic : opcodes.after_demosaic)
                .push_back(std::move(*map));
            continue;
        }
        if (id == kWarpRectilinear && number == 3 && !opcodes.warp) {
            std::optional<Warp> w = warp(body);
            if (!w) {
                return fail(error, damaged);
            }
            opcodes.warp = *w;
            continue;
        }
        if (!may_skip) {
            return fail(error, "a DNG asking for " + name_of(id) + " in opcode list " +
                                   std::to_string(number) + ", which rummage does not apply");
        }
    }
    return true;
}

std::optional<Opcodes> read_opcodes(const tiff::Ifd& ifd, std::string& error) {
    Opcodes opcodes;
    const std::array<std::uint16_t, 3> tags{tiff::tag::kOpcodeList1, tiff::tag::kOpcodeList2,
                                            tiff::tag::kOpcodeList3};
    for (int number = 1; number <= 3; ++number) {
        const std::uint16_t tag = tags.at(static_cast<std::size_t>(number - 1));
        if (!ifd.has(tag)) {
            continue;
        }
        const std::vector<std::uint8_t> bytes = ifd.bytes(tag, kMaxListBytes);
        if (bytes.empty()) {
            error = "a DNG with an unreadable opcode list";
            return std::nullopt;
        }
        if (!read_list(bytes, number, opcodes, error)) {
            return std::nullopt;
        }
    }
    return opcodes;
}

}  // namespace rummage::formats::image::dng
