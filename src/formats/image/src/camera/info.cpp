#include "info.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../jpeg_segments.hpp"
#include "../tiff/container.hpp"
#include "previews.hpp"

namespace rummage::formats::image::camera {
namespace {

constexpr std::size_t kExifHead = std::size_t{256} * 1024;

constexpr std::uint16_t kMake = 0x010F;
constexpr std::uint16_t kModel = 0x0110;
constexpr std::uint16_t kSoftware = 0x0131;
constexpr std::uint16_t kExifIfd = 0x8769;
constexpr std::uint16_t kExposureTime = 0x829A;
constexpr std::uint16_t kFNumber = 0x829D;
constexpr std::uint16_t kExposureProgram = 0x8822;
constexpr std::uint16_t kIso = 0x8827;
constexpr std::uint16_t kDateTimeOriginal = 0x9003;
constexpr std::uint16_t kExposureBias = 0x9204;
constexpr std::uint16_t kMeteringMode = 0x9207;
constexpr std::uint16_t kFlash = 0x9209;
constexpr std::uint16_t kFocalLength = 0x920A;
constexpr std::uint16_t kWhiteBalance = 0xA403;
constexpr std::uint16_t kLensModel = 0xA434;
constexpr std::uint16_t kPanasonicJpeg = 0x002E;

// One IFD where values may be read: what exiv2 called a group (Exif.Image, Exif.Photo).
using Group = std::optional<tiff::Ifd>;

// The first of groups holding tag.
const tiff::Ifd* holding(std::span<const Group> groups, std::uint16_t tag) {
    for (const Group& group : groups) {
        if (group && group->has(tag)) {
            return &*group;
        }
    }
    return nullptr;
}

// An ASCII value up to its first NUL, without the spaces and tabs at its end (as rummage showed
// exiv2's).
std::string text(std::span<const Group> groups, std::uint16_t tag) {
    const tiff::Ifd* ifd = holding(groups, tag);
    if (ifd == nullptr) {
        return {};
    }
    const std::vector<std::uint8_t> bytes = ifd->bytes(tag, 4096);
    std::string value(bytes.begin(), bytes.end());
    value.resize(std::min(value.size(), value.find('\0')));
    const std::size_t end = value.find_last_not_of(" \t");
    value.resize(end == std::string::npos ? 0 : end + 1);
    return value;
}

// A value's first rational (numerator, denominator) as stored, and whether it is of type
// RATIONAL (unsigned); an integer gives (n, 1).
struct Fraction {
    std::int64_t numerator{0};
    std::int64_t denominator{1};
    bool unsigned_rational{false};
};

std::optional<Fraction> fraction(std::span<const Group> groups, std::uint16_t tag) {
    const tiff::Ifd* ifd = holding(groups, tag);
    if (ifd == nullptr) {
        return std::nullopt;
    }
    const tiff::Entry* entry = ifd->find(tag);
    if (entry == nullptr || entry->count == 0) {
        return std::nullopt;
    }
    const auto type = static_cast<tiff::Type>(entry->type);
    if (type == tiff::Type::rational || type == tiff::Type::srational) {
        const std::vector<std::uint8_t> bytes = ifd->bytes(tag, 8);
        if (bytes.size() < 8) {
            return std::nullopt;
        }
        const tiff::Container& c = ifd->container();
        const std::uint32_t n = c.u32(std::span{bytes}.first(4));
        const std::uint32_t d = c.u32(std::span{bytes}.subspan(4, 4));
        if (type == tiff::Type::rational) {
            return Fraction{.numerator = n, .denominator = d, .unsigned_rational = true};
        }
        return Fraction{.numerator = static_cast<std::int32_t>(n),
                        .denominator = static_cast<std::int32_t>(d),
                        .unsigned_rational = false};
    }
    const std::optional<std::uint64_t> number = ifd->number(tag);
    if (!number) {
        return std::nullopt;
    }
    return Fraction{.numerator = static_cast<std::int64_t>(*number),
                    .denominator = 1,
                    .unsigned_rational = false};
}

std::optional<std::int64_t> integer(std::span<const Group> groups, std::uint16_t tag) {
    const tiff::Ifd* ifd = holding(groups, tag);
    if (ifd == nullptr) {
        return std::nullopt;
    }
    const tiff::Entry* entry = ifd->find(tag);
    if (entry == nullptr || entry->count == 0) {
        return std::nullopt;
    }
    if (const std::optional<std::uint64_t> value = ifd->number(tag)) {
        return static_cast<std::int64_t>(*value);
    }
    return std::nullopt;
}

// A float as C++ streams print it, the way exiv2 did: general with 6 significant digits
// (digits 6), general with 2 (digits 2), or fixed with one decimal (digits 0).
std::string format(double value, int digits) {
    std::array<char, 64> buffer{};
    int length = 0;
    if (digits == 6) {
        length = std::snprintf(buffer.data(), buffer.size(), "%g", value);  // NOLINT: vararg
    } else if (digits == 2) {
        length = std::snprintf(buffer.data(), buffer.size(), "%.2g", value);  // NOLINT: vararg
    } else {
        length = std::snprintf(buffer.data(), buffer.size(), "%.1f", value);  // NOLINT: vararg
    }
    return length > 0 ? std::string(buffer.data(), static_cast<std::size_t>(length))
                      : std::string{};
}

std::string ratio(std::int64_t numerator, std::int64_t denominator) {
    return std::to_string(numerator) + "/" + std::to_string(denominator);
}

// "1/2000 s": a fraction of 1 when it is one, else the seconds as a number.
std::string exposure_time(std::optional<Fraction> f) {
    if (!f) {
        return {};
    }
    if (!f->unsigned_rational) {
        return "(" + ratio(f->numerator, f->denominator) + ")";
    }
    const auto n = static_cast<std::uint32_t>(f->numerator);
    const auto d = static_cast<std::uint32_t>(f->denominator);
    if (n == 0 || d == 0) {
        return "(" + ratio(n, d) + ")";
    }
    if (n == d) {
        return "1 s";
    }
    if (d % n == 0) {
        return "1/" + std::to_string(d / n) + " s";
    }
    return format(static_cast<double>(static_cast<float>(n) / static_cast<float>(d)), 6) + " s";
}

// "F5.6": two significant digits.
std::string aperture(std::optional<Fraction> f) {
    if (!f) {
        return {};
    }
    const auto n = static_cast<std::int32_t>(f->numerator);
    const auto d = static_cast<std::int32_t>(f->denominator);
    if (d == 0) {
        return "(" + ratio(f->numerator, f->denominator) + ")";
    }
    return "F" + format(static_cast<double>(static_cast<float>(n) / static_cast<float>(d)), 2);
}

// "25.0 mm": one decimal.
std::string focal_length(std::optional<Fraction> f) {
    if (!f) {
        return {};
    }
    const auto n = static_cast<std::int32_t>(f->numerator);
    const auto d = static_cast<std::int32_t>(f->denominator);
    if (d == 0) {
        return "(" + ratio(f->numerator, f->denominator) + ")";
    }
    return format(static_cast<double>(static_cast<float>(n) / static_cast<float>(d)), 0) + " mm";
}

// "+7/10 EV", reduced; "0 EV".
std::string exposure_bias(std::optional<Fraction> f) {
    if (!f) {
        return {};
    }
    const auto n = static_cast<std::int32_t>(f->numerator);
    const auto d = static_cast<std::int32_t>(f->denominator);
    if (n == 0 || n == INT32_MIN) {
        return "0 EV";
    }
    if (d <= 0) {
        return "(" + std::to_string(n) + "/" + std::to_string(d) + ")";
    }
    const std::int64_t g = std::gcd(std::int64_t{n}, std::int64_t{d});
    const std::int64_t num = std::abs(std::int64_t{n}) / g;
    const std::int64_t den = d / g;
    std::string out = (n < 0 ? "-" : "+") + std::to_string(num);
    if (den != 1) {
        out += "/" + std::to_string(den);
    }
    return out + " EV";
}

struct Label {
    std::int64_t value;
    std::string_view text;
};

constexpr std::array<Label, 9> kProgramLabels{{{0, "Not defined"},
                                               {1, "Manual"},
                                               {2, "Auto"},
                                               {3, "Aperture priority"},
                                               {4, "Shutter priority"},
                                               {5, "Creative program"},
                                               {6, "Action program"},
                                               {7, "Portrait mode"},
                                               {8, "Landscape mode"}}};
constexpr std::array<Label, 8> kMeteringLabels{{{0, "Unknown"},
                                                {1, "Average"},
                                                {2, "Center weighted average"},
                                                {3, "Spot"},
                                                {4, "Multi-spot"},
                                                {5, "Multi-segment"},
                                                {6, "Partial"},
                                                {255, "Other"}}};
constexpr std::array<Label, 26> kFlashLabels{{
    {0x00, "No flash"},
    {0x01, "Fired"},
    {0x05, "Fired, return light not detected"},
    {0x07, "Fired, return light detected"},
    {0x08, "Yes, did not fire"},
    {0x09, "Yes, compulsory"},
    {0x0d, "Yes, compulsory, return light not detected"},
    {0x0f, "Yes, compulsory, return light detected"},
    {0x10, "No, compulsory"},
    {0x14, "No, did not fire, return light not detected"},
    {0x18, "No, auto"},
    {0x19, "Yes, auto"},
    {0x1d, "Yes, auto, return light not detected"},
    {0x1f, "Yes, auto, return light detected"},
    {0x20, "No flash function"},
    {0x41, "Yes, red-eye reduction"},
    {0x45, "Yes, red-eye reduction, return light not detected"},
    {0x47, "Yes, red-eye reduction, return light detected"},
    {0x49, "Yes, compulsory, red-eye reduction"},
    {0x4d, "Yes, compulsory, red-eye reduction, return light not detected"},
    {0x4f, "Yes, compulsory, red-eye reduction, return light detected"},
    {0x50, "No, red-eye reduction"},
    {0x58, "No, auto, red-eye reduction"},
    {0x59, "Yes, auto, red-eye reduction"},
    {0x5d, "Yes, auto, red-eye reduction, return light not detected"},
    {0x5f, "Yes, auto, red-eye reduction, return light detected"},
}};
constexpr std::array<Label, 2> kWhiteBalanceLabels{{{0, "Auto"}, {1, "Manual"}}};

// A value's label, "(n)" for one not in the table.
std::string label(std::span<const Label> labels, std::optional<std::int64_t> value) {
    if (!value) {
        return {};
    }
    for (const Label& l : labels) {
        if (l.value == *value) {
            return std::string{l.text};
        }
    }
    return "(" + std::to_string(*value) + ")";
}

// "2026:08:31 16:34:10" -> "2026-08-31 16:34".
std::string date_of(std::string exif) {
    if (exif.size() < 16 || exif[4] != ':' || exif[7] != ':') {
        return exif;
    }
    exif[4] = '-';
    exif[7] = '-';
    exif.resize(16);
    return exif;
}

// Model alone when it names the maker ("Canon EOS 5DS") or the maker is long ("OM Digital
// Solutions" makes "OM-1"); else both ("NIKON CORPORATION" + "NIKON D800" -> the model).
std::string camera_of(const std::string& make, const std::string& model) {
    if (model.empty()) {
        return make;
    }
    const std::string_view first_word = std::string_view{make}.substr(0, make.find(' '));
    if (make.empty() || model.find(first_word) != std::string::npos ||
        make.find(' ') != std::string::npos) {
        return model;
    }
    return make + " " + model;
}

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at) {
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
           (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
}

std::uint32_t le16(std::span<const std::uint8_t> b, std::size_t at) {
    return b[at] | (std::uint32_t{b[at + 1]} << 8);
}

std::uint32_t le24(std::span<const std::uint8_t> b, std::size_t at) {
    return le16(b, at) | (std::uint32_t{b[at + 2]} << 16);
}

// The size PNG, GIF and WebP keep in their headers.
std::optional<std::pair<std::uint32_t, std::uint32_t>> header_size(
    std::span<const std::uint8_t> head) {
    const auto starts = [&](std::size_t at, std::string_view text) {
        return head.size() >= at + text.size() &&
               std::equal(text.begin(), text.end(), head.begin() + static_cast<std::ptrdiff_t>(at),
                          [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; });
    };
    if (starts(0, "\x89PNG\r\n\x1a\n") && starts(12, "IHDR") && head.size() >= 24) {
        return std::pair{be32(head, 16), be32(head, 20)};
    }
    if ((starts(0, "GIF87a") || starts(0, "GIF89a")) && head.size() >= 10) {
        return std::pair{le16(head, 6), le16(head, 8)};
    }
    if (starts(0, "RIFF") && starts(8, "WEBP") && head.size() >= 30) {
        if (starts(12, "VP8X")) {
            return std::pair{le24(head, 24) + 1, le24(head, 27) + 1};
        }
        if (starts(12, "VP8 ")) {
            return std::pair{le16(head, 26) & 0x3FFFU, le16(head, 28) & 0x3FFFU};
        }
        if (starts(12, "VP8L")) {
            const std::uint32_t bits = le16(head, 21) | (le16(head, 23) << 16);
            return std::pair{(bits & 0x3FFFU) + 1, ((bits >> 14) & 0x3FFFU) + 1};
        }
    }
    return std::nullopt;
}

// The EXIF of a JPEG whose first bytes are head: its IFD0 and EXIF IFD (the containers keep
// head, which must outlive them).
struct JpegExif {
    Group image;
    Group photo;
};

JpegExif jpeg_exif(std::span<const std::uint8_t> head) {
    JpegExif out;
    const std::optional<detail::JpegExif> exif = detail::jpeg_exif(head);
    if (!exif) {
        return out;
    }
    const tiff::Source source{exif->tiff};
    const std::optional<tiff::Container> container = tiff::Container::open(source);
    if (!container) {
        return out;
    }
    const std::vector<tiff::Ifd> chain = container->chain(1);
    if (chain.empty()) {
        return out;
    }
    out.image = chain.front();
    if (const auto at = chain.front().number(kExifIfd)) {
        out.photo = container->ifd(*at);
    }
    return out;
}

}  // namespace

picture::PhotoInfo read_info(const detail::FdFile& file) {
    picture::PhotoInfo info;
    const std::vector<std::uint8_t> head = file.read(0, 64);
    if (const auto size = header_size(head)) {
        info.width = size->first;
        info.height = size->second;
        return info;
    }
    const std::optional<CameraFile> camera = read(file, false);
    if (!camera) {
        return info;
    }
    // Where each value is looked for, in order: the raw's own IFDs, then its JPEG's (exiv2 added
    // the JPEG's values a raw lacked: RW2 keeps its camera's name only there).
    std::array<Group, 2> image{};
    std::array<Group, 2> photo{};
    std::vector<std::uint8_t> jpeg_head;  // the JPEG's EXIF, for the groups read from it
    std::optional<std::uint64_t> jpeg_at;
    std::uint64_t jpeg_length = 0;
    if (camera->type == Type::jpeg) {
        jpeg_at = 0;
        jpeg_length = file.size();
    } else if (camera->type == Type::raf) {
        const std::vector<std::uint8_t> header = file.read_exactly(0, 92);
        if (!header.empty()) {
            jpeg_at = be32(header, 84);
            jpeg_length = be32(header, 88);
        }
    }
    const tiff::Source raw_source{file};
    const std::optional<tiff::Container> raw =
        camera->type == Type::jpeg || camera->type == Type::raf
            ? std::nullopt
            : tiff::Container::open(raw_source, true);
    if (raw) {
        const std::vector<tiff::Ifd> chain = raw->chain(1);
        if (!chain.empty()) {
            // A Panasonic raw's IFD0 holds its own tags (exiv2's PanasonicRaw group), not the
            // camera's name: that comes from the JPEG in tag 0x2E.
            if (camera->type == Type::rw2) {
                if (const tiff::Entry* entry = chain.front().find(kPanasonicJpeg);
                    entry != nullptr && !entry->is_inline) {
                    jpeg_at = entry->offset;
                    jpeg_length = entry->count;
                }
            } else {
                image[0] = chain.front();
            }
            if (const auto at = chain.front().number(kExifIfd)) {
                photo[0] = raw->ifd(*at);
            }
        }
    }
    JpegExif from_jpeg;
    if (jpeg_at) {
        jpeg_head = file.read(
            *jpeg_at, static_cast<std::size_t>(std::min<std::uint64_t>(jpeg_length, kExifHead)));
        from_jpeg = jpeg_exif(jpeg_head);
        image[1] = from_jpeg.image;
        photo[1] = from_jpeg.photo;
    }

    info.width = camera->full_width;
    info.height = camera->full_height;
    const std::optional<std::int64_t> orientation = integer(image, tiff::tag::kOrientation);
    if (orientation && *orientation >= 5 && *orientation <= 8) {
        std::swap(info.width, info.height);
    }
    info.camera = camera_of(text(image, kMake), text(image, kModel));
    info.lens = text(photo, kLensModel);
    info.exposure_time = exposure_time(fraction(photo, kExposureTime));
    info.aperture = aperture(fraction(photo, kFNumber));
    const std::optional<std::int64_t> iso = integer(photo, kIso);
    info.iso = iso ? std::to_string(*iso) : std::string{};
    info.focal_length = focal_length(fraction(photo, kFocalLength));
    info.taken = date_of(text(photo, kDateTimeOriginal));
    info.program = label(kProgramLabels, integer(photo, kExposureProgram));
    info.exposure_bias = exposure_bias(fraction(photo, kExposureBias));
    info.metering = label(kMeteringLabels, integer(photo, kMeteringMode));
    info.flash = label(kFlashLabels, integer(photo, kFlash));
    info.white_balance = label(kWhiteBalanceLabels, integer(photo, kWhiteBalance));
    info.software = text(image, kSoftware);
    return info;
}

}  // namespace rummage::formats::image::camera
