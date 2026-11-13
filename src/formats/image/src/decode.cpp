#include "rummage/formats/image/decode.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

#include "decoders.hpp"
#include "jpeg_segments.hpp"
#include "tiff/decode.hpp"

namespace rummage::formats::image {
namespace {

bool starts_with(std::span<const std::uint8_t> data, std::span<const std::uint8_t> prefix) {
    return data.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), data.begin());
}

constexpr std::array<std::uint8_t, 8> kPng{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr std::array<std::uint8_t, 3> kJpeg{0xFF, 0xD8, 0xFF};
constexpr std::array<std::uint8_t, 2> kJxlCodestream{0xFF, 0x0A};
constexpr std::array<std::uint8_t, 12> kJxlContainer{0x00, 0x00, 0x00, 0x0C, 'J',  'X',
                                                     'L',  ' ',  0x0D, 0x0A, 0x87, 0x0A};
constexpr std::array<std::uint8_t, 6> kGif87{'G', 'I', 'F', '8', '7', 'a'};
constexpr std::array<std::uint8_t, 6> kGif89{'G', 'I', 'F', '8', '9', 'a'};
constexpr std::array<std::uint8_t, 4> kRiff{'R', 'I', 'F', 'F'};
constexpr std::array<std::uint8_t, 4> kWebp{'W', 'E', 'B', 'P'};
constexpr std::array<std::uint8_t, 6> kExif{'E', 'x', 'i', 'f', 0, 0};
constexpr std::array<std::uint8_t, 4> kTiffLittle{'I', 'I', 42, 0};
constexpr std::array<std::uint8_t, 4> kTiffBig{'M', 'M', 0, 42};
constexpr std::size_t kSniffBytes = 16;  // enough for sniff()

// The EXIF orientation of a JPEG: wuffs does not report JPEG metadata, so it is read from the
// APP1 "Exif" segment here.
std::uint8_t jpeg_orientation(std::span<const std::uint8_t> data) {
    const std::optional<detail::JpegExif> exif = detail::jpeg_exif(data);
    return exif ? exif_orientation(exif->tiff) : 1;
}

}  // namespace

std::string_view format_name(Format format) noexcept {
    switch (format) {
        case Format::png:
            return "PNG";
        case Format::jpeg:
            return "JPEG";
        case Format::gif:
            return "GIF";
        case Format::webp:
            return "WebP";
        case Format::jxl:
            return "JPEG XL";
        case Format::pnm:
            return "PNM";
        case Format::tiff:
            return "TIFF";
        case Format::unknown:
            break;
    }
    return "unknown";
}

Format sniff(std::span<const std::uint8_t> head) noexcept {
    if (starts_with(head, kPng)) {
        return Format::png;
    }
    if (starts_with(head, kJpeg)) {
        return Format::jpeg;
    }
    if (starts_with(head, kGif87) || starts_with(head, kGif89)) {
        return Format::gif;
    }
    if (starts_with(head, kRiff) && head.size() >= 12 && starts_with(head.subspan(8), kWebp)) {
        return Format::webp;
    }
    if (starts_with(head, kJxlCodestream) || starts_with(head, kJxlContainer)) {
        return Format::jxl;
    }
    // "P5" or "P6" and whitespace: binary PGM or PPM.
    if (head.size() >= 3 && head[0] == 'P' && (head[1] == '5' || head[1] == '6') &&
        (head[2] == ' ' || head[2] == '\t' || head[2] == '\n' || head[2] == '\r')) {
        return Format::pnm;
    }
    // TIFF and BigTIFF, either byte order.
    if (head.size() >= 4 &&
        ((head[0] == 'I' && head[1] == 'I' && (head[2] == 42 || head[2] == 43) && head[3] == 0) ||
         (head[0] == 'M' && head[1] == 'M' && head[2] == 0 && (head[3] == 42 || head[3] == 43)))) {
        return Format::tiff;
    }
    return Format::unknown;
}

std::uint8_t exif_orientation(std::span<const std::uint8_t> exif) noexcept {
    if (starts_with(exif, kExif)) {
        exif = exif.subspan(6);
    }
    if (!starts_with(exif, kTiffLittle) && !starts_with(exif, kTiffBig)) {
        return 1;
    }
    const tiff::Source source{exif};
    const std::optional<tiff::Container> container = tiff::Container::open(source);
    const std::optional<tiff::Ifd> first =
        container ? container->ifd(container->first_ifd()) : std::nullopt;
    const std::uint64_t value = first ? first->number(tiff::tag::kOrientation).value_or(1) : 1;
    return value >= 1 && value <= 8 ? static_cast<std::uint8_t>(value) : 1;
}

Decoded decode(std::span<const std::uint8_t> data, const DecodeOptions& options) {
    Decoded result;
    result.format = sniff(data);
    switch (result.format) {
        case Format::jpeg:
            detail::decode_wuffs(data, options, result);
            result.orientation = jpeg_orientation(data);
            break;
        case Format::png:
        case Format::gif:
            detail::decode_wuffs(data, options, result);
            break;
        case Format::webp:
            detail::decode_webp(data, options, result);
            break;
        case Format::jxl:
            detail::decode_jxl(data, options, result);
            break;
        case Format::pnm:
            detail::decode_pnm(data, options, result);
            break;
        case Format::tiff:
            result = tiff::decode(tiff::Source{data}, options);
            break;
        case Format::unknown:
            result.error = data.empty() ? "empty file" : "not an image format rummage decodes";
            result.problem = data.empty() ? Problem::damaged : Problem::unknown_format;
            return result;
    }
    if (!result.error.empty()) {
        result.image = {};
        result.problem = result.error == detail::kTooLarge ? Problem::too_large : Problem::damaged;
        return result;
    }
    if (options.apply_orientation) {
        result.image = apply_orientation(std::move(result.image), result.orientation);
    }
    return result;
}

Decoded decode_file(const std::filesystem::path& path, const DecodeOptions& options) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        Decoded result;
        result.error = error.message();
        result.problem = Problem::unreadable;
        return result;
    }
    // The head first: a file of a format not decoded here (a raw file of 50 MB) is not read.
    const std::size_t head = std::min<std::size_t>(size, kSniffBytes);
    std::vector<std::uint8_t> data(head);
    std::ifstream file{path, std::ios::binary};
    const auto read = [&](std::size_t from, std::size_t count) {
        return static_cast<bool>(
            file.read(reinterpret_cast<char*>(data.data() + from),  // NOLINT: bytes to char
                      static_cast<std::streamsize>(count)));
    };
    if (!read(0, head)) {
        Decoded result;
        result.error = "cannot read the file";
        result.problem = Problem::unreadable;
        return result;
    }
    if (sniff(data) == Format::unknown) {
        return decode(data, options);  // fails with the reason
    }
    data.resize(size);
    if (!read(head, size - head)) {
        Decoded result;
        result.error = "cannot read the file";
        result.problem = Problem::unreadable;
        return result;
    }
    return decode(data, options);
}

}  // namespace rummage::formats::image
