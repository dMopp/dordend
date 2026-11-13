#include "rummage/formats/image/load.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "dng/develop.hpp"
#include "fd_file.hpp"
#include "jpeg_segments.hpp"
#include "rummage/formats/image/preview.hpp"
#include "rummage/picture/scale.hpp"
#include "tiff/decode.hpp"

namespace rummage::formats::image {

using picture::scale_to_fit;

namespace {

std::atomic<std::uint64_t> preview_reads{0};

// Bytes read to tell the format, and to find a JPEG's EXIF segment (after the few segments
// before it).
constexpr std::size_t kHead = std::size_t{64} * 1024;

// What a file is, from its first bytes.
enum class Kind : std::uint8_t {
    image,       // PNG, GIF, WebP, JPEG XL, PGM/PPM: decoded whole, never a stored preview
    jpeg,        // decoded whole; a camera's (with EXIF) may carry a larger preview first
    container,   // TIFF and the raw formats built like it, RAF, CR3: previews only
    plain_tiff,  // a TIFF that is not a camera raw: decoded here, read piece by piece
    no_reader,   // a raw format with no preview rummage reads (Sigma X3F, Canon CRW, Minolta MRW)
    unknown,     // nothing known: only a camera raw's MIME type led here
};

bool starts_with(std::span<const std::uint8_t> data, std::string_view prefix) {
    return data.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), data.begin(),
                      [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; });
}

Kind kind_of(std::span<const std::uint8_t> head) {
    switch (sniff(head)) {
        case Format::jpeg:
            return Kind::jpeg;
        case Format::png:
        case Format::gif:
        case Format::webp:
        case Format::jxl:
        case Format::pnm:
            return Kind::image;
        case Format::tiff:  // a raw or a plain TIFF: told apart in load(), which has the file
        case Format::unknown:
            break;
    }
    using namespace std::string_view_literals;
    // TIFF (also DNG, NEF, CR2, ARW, PEF, SRW, 3FR, ERF), ORF, RW2 and old Panasonic RAW.
    for (const std::string_view magic :
         {"II*\0"sv, "MM\0*"sv, "II+\0"sv, "MM\0+"sv, "IIRO"sv, "IIRS"sv, "MMOR"sv, "IIU\0"sv}) {
        if (starts_with(head, magic)) {
            return Kind::container;
        }
    }
    if (starts_with(head, "FUJIFILM") ||
        (head.size() >= 12 && starts_with(head.subspan(4), "ftypcrx "))) {
        return Kind::container;
    }
    // Decided with the author (2026-11-08): the old Canon CRW and Minolta MRW get the plain
    // message, as Sigma's X3F.
    if (starts_with(head, "FOVb") || starts_with(head, "\0MRM"sv) ||
        starts_with(head, "II\x1a\0\0\0HEAPCCDR"sv)) {
        return Kind::no_reader;
    }
    return Kind::unknown;
}

Failure failure_of(Problem problem) {
    switch (problem) {
        case Problem::none:
            return Failure::none;
        case Problem::unknown_format:
            return Failure::unknown_format;
        case Problem::too_large:
            return Failure::too_large;
        case Problem::damaged:
            return Failure::damaged;
        case Problem::unreadable:
            return Failure::unreadable;
    }
    return Failure::damaged;
}

// Whether a preview of width x height is enough for a box of max_width x max_height: it would be
// stretched at most 4/3 to fill it (either way round: the picture may be turned). A DNG with
// none enough is developed from its sensor data.
bool enough(std::uint32_t width, std::uint32_t height, std::uint32_t max_width,
            std::uint32_t max_height) {
    if (width == 0 || height == 0) {
        return false;
    }
    const double long_side = std::max(width, height);
    const double short_side = std::min(width, height);
    const double stretch = std::min(std::max(max_width, max_height) / long_side,
                                    std::min(max_width, max_height) / short_side);
    return stretch <= 4.0 / 3.0;
}

Loaded failed(Failure failure, std::string detail) {
    Loaded result;
    result.failure = failure;
    result.detail = std::move(detail);
    return result;
}

}  // namespace

bool recognises(std::span<const std::uint8_t> head) { return kind_of(head) != Kind::unknown; }

std::uint64_t embedded_preview_reads() noexcept {
    return preview_reads.load(std::memory_order_relaxed);
}

Loaded load(const LoadRequest& request) {
    const int fd = ::open(request.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return failed(Failure::unreadable,
                      std::error_code{errno, std::generic_category()}.message());
    }
    Loaded result = load(fd, request.max_width, request.max_height);
    ::close(fd);
    return result;
}

Loaded load(int fd, std::uint32_t max_width, std::uint32_t max_height) {
    const std::optional<detail::FdFile> file = detail::FdFile::of(fd);
    if (!file) {
        return failed(Failure::unreadable, "cannot read the file");
    }
    const std::vector<std::uint8_t> head = file->read(0, kHead);
    if (head.empty()) {
        return failed(file->size() == 0 ? Failure::damaged : Failure::unreadable,
                      file->size() == 0 ? "empty file" : "cannot read the file");
    }
    Kind kind = kind_of(head);
    if (kind == Kind::container && sniff(head) == Format::tiff &&
        !tiff::is_camera_raw(tiff::Source{*file})) {
        kind = Kind::plain_tiff;
    }
    if (kind == Kind::no_reader) {
        return failed(Failure::no_readable_preview, "a raw format rummage does not read");
    }

    Loaded result;
    const std::uint32_t side = std::max(max_width, max_height);
    Image picture;
    std::uint8_t orientation = 1;
    std::optional<Failure> tiff_failure;  // a TIFF that did not decode, if previews fail too

    if (kind == Kind::plain_tiff) {
        Decoded decoded = tiff::decode(tiff::Source{*file}, {.apply_orientation = false});
        if (decoded.error.empty()) {
            picture = std::move(decoded.image);
            orientation = decoded.orientation;
            result.full_width = picture.width;
            result.full_height = picture.height;
        } else if (decoded.problem == Problem::too_large) {
            return failed(Failure::too_large, std::move(decoded.error));
        } else {
            // Not a TIFF this reader shows: maybe a camera raw it did not recognise, whose
            // stored previews still show it.
            kind = Kind::container;
            tiff_failure = failure_of(decoded.problem);
            result.detail = std::move(decoded.error);
        }
    }
    // A preview the file carries: camera JPEGs (only those with EXIF can have one) and raw files.
    EmbeddedPreview preview;
    const bool camera_jpeg = kind == Kind::jpeg && detail::jpeg_exif(head).has_value();
    if (kind == Kind::container || kind == Kind::unknown || camera_jpeg) {
        preview_reads.fetch_add(1, std::memory_order_relaxed);
        preview = embedded_preview(fd, side);
    }
    const auto from_preview = [&] {
        Decoded decoded = decode_preview(preview);
        if (!decoded.error.empty()) {
            result.detail = decoded.error;
            return false;
        }
        picture = std::move(decoded.image);
        orientation = preview.orientation;
        result.from_preview = true;
        return true;
    };
    // A DNG whose previews are too small: developed (rummage's own code), else the previews.
    if (picture.empty() && kind == Kind::container && sniff(head) == Format::tiff &&
        !enough(preview.width, preview.height, max_width, max_height)) {
        const tiff::Source source{*file};
        if (const std::optional<tiff::Container> container = tiff::Container::open(source)) {
            std::string why;
            Problem problem{};
            if (std::optional<dng::Developed> developed = dng::develop(
                    *container, {.look = true, .max_width = max_width, .max_height = max_height},
                    why, problem)) {
                picture = std::move(developed->image);
                orientation = developed->orientation;
                result.full_width = developed->full_width;
                result.full_height = developed->full_height;
            }
        }
    }
    if (picture.empty() && !preview.data.empty() &&
        std::max(preview.width, preview.height) >= side) {
        from_preview();
    }
    if (picture.empty() && (kind == Kind::image || kind == Kind::jpeg)) {
        const std::vector<std::uint8_t> whole = file->read_exactly(0, file->size());
        Decoded decoded;
        if (whole.empty()) {
            decoded.error = "cannot read the file";
            decoded.problem = Problem::unreadable;
        } else {
            decoded = decode(whole, {.apply_orientation = false});
        }
        if (decoded.error.empty()) {
            picture = std::move(decoded.image);
            orientation = decoded.orientation;
            result.full_width = picture.width;
            result.full_height = picture.height;
        } else if (preview.data.empty() || !from_preview()) {
            return failed(failure_of(decoded.problem), std::move(decoded.error));
        }
    }
    if (picture.empty()) {
        // A raw file: the largest preview it has, whatever its size.
        if (preview.data.empty() || !from_preview()) {
            std::string detail = preview.error;
            if (detail.empty()) {
                detail = result.detail.empty() ? "no preview in the file" : result.detail;
            }
            if (tiff_failure) {
                return failed(*tiff_failure, std::move(detail));
            }
            return failed(
                kind == Kind::unknown ? Failure::unknown_format : Failure::no_readable_preview,
                std::move(detail));
        }
    }
    if (result.full_width == 0 && preview.full_width > 0) {
        result.full_width = preview.full_width;
        result.full_height = preview.full_height;
    }
    if (orientation >= 5) {
        std::swap(result.full_width, result.full_height);
    }
    result.detail.clear();
    // Scaled first, turned after: the turn then works on screen-sized pixels.
    result.image = apply_orientation(scale_to_fit(picture, max_width, max_height), orientation);
    return result;
}

}  // namespace rummage::formats::image
