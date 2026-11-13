#include "previews.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>

#include "../jpeg_segments.hpp"

namespace rummage::formats::image::camera {
namespace {

// Bytes read for a JPEG's EXIF (at most 64 KB, after the few segments before it).
constexpr std::size_t kExifHead = std::size_t{256} * 1024;
constexpr std::size_t kMaxSubIfds = 16;
constexpr std::size_t kMaxStrips = std::size_t{1} << 16;
constexpr std::size_t kPrimaryGroups = 9;  // IFD0's SubIFDs looked at for the main image

constexpr std::uint16_t kJpegOffset = 0x0201;
constexpr std::uint16_t kJpegLength = 0x0202;
constexpr std::uint16_t kExifIfd = 0x8769;
constexpr std::uint16_t kMakerNote = 0x927C;
constexpr std::uint16_t kPixelXDimension = 0xA002;
constexpr std::uint16_t kPixelYDimension = 0xA003;
constexpr std::uint16_t kMake = 0x010F;
constexpr std::uint16_t kPanasonicSensorWidth = 0x0002;
constexpr std::uint16_t kPanasonicSensorHeight = 0x0003;
constexpr std::uint16_t kPanasonicJpeg = 0x002E;
constexpr std::uint16_t kOlympusThumbnail = 0x0100;
constexpr std::uint16_t kOlympusCameraSettings = 0x2020;
constexpr std::uint16_t kOlympusPreviewValid = 0x0100;
constexpr std::uint16_t kOlympusPreviewStart = 0x0101;
constexpr std::uint16_t kOlympusPreviewLength = 0x0102;
constexpr std::uint16_t kNikonPreviewIfd = 0x0011;
constexpr std::uint16_t kSamsungPreviewIfd = 0x0035;
constexpr std::uint16_t kRafFullSize = 0x0100;
constexpr std::uint16_t kCanonSlices = 0xC640;

constexpr std::uint64_t kOldJpeg = 6;
constexpr std::uint64_t kRgb = 2;
constexpr std::uint64_t kYCbCr = 6;
constexpr std::uint64_t kCfa = 32803;
constexpr std::uint64_t kLinearRaw = 34892;

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at) {
    return (std::uint32_t{b[at]} << 24) | (std::uint32_t{b[at + 1]} << 16) |
           (std::uint32_t{b[at + 2]} << 8) | b[at + 3];
}

std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}

bool starts_with(std::span<const std::uint8_t> bytes, std::string_view text) {
    return bytes.size() >= text.size() && std::memcmp(bytes.data(), text.data(), text.size()) == 0;
}

std::uint32_t narrow(std::optional<std::uint64_t> value) {
    return value && *value <= UINT32_MAX ? static_cast<std::uint32_t>(*value) : 0;
}

class Reader {
public:
    Reader(const detail::FdFile& file, CameraFile& out) : file_{file}, out_{out} {}

    // A JPEG stream at offset in the file, length long (the same one found twice is kept once).
    void jpeg(std::uint64_t offset, std::uint64_t length) {
        if (length <= 4 || offset > file_.size() || length > file_.size() - offset) {
            return;
        }
        const bool known = std::ranges::any_of(
            out_.previews, [&](const Found& f) { return f.offset == offset && !f.ifd; });
        if (!known) {
            out_.previews.push_back(
                {.offset = offset, .length = length, .ifd = std::nullopt, .width = 0, .height = 0});
        }
    }

    // The previews an IFD holds: a JPEG by JPEGInterchangeFormat, or as an old-style JPEG strip
    // (CR2's IFD0), or, when tiffs, a reduced 8-bit RGB image of its own (read from the file
    // only: base is where the IFD's structure starts in the file).
    void ifd(const tiff::Ifd& ifd, std::uint64_t base, bool tiffs) {
        const std::uint64_t photometric = ifd.number(tiff::tag::kPhotometric).value_or(0);
        if (photometric == kCfa || photometric == kLinearRaw || ifd.has(kCanonSlices)) {
            return;  // sensor data (a CR2's is a lossless JPEG in slices)
        }
        const std::optional<std::uint64_t> offset = ifd.number(kJpegOffset);
        const std::optional<std::uint64_t> length = ifd.number(kJpegLength);
        if (offset && length) {
            jpeg(base + *offset, *length);
            return;
        }
        const std::vector<std::uint64_t> strips = ifd.numbers(tiff::tag::kStripOffsets, kMaxStrips);
        const std::vector<std::uint64_t> counts =
            ifd.numbers(tiff::tag::kStripByteCounts, kMaxStrips);
        const std::uint64_t compression = ifd.number(tiff::tag::kCompression).value_or(1);
        if (compression == kOldJpeg) {
            if (strips.size() == 1 && counts.size() == 1) {
                jpeg(base + strips[0], counts[0]);
            }
            return;
        }
        const bool reduced = (ifd.number(tiff::tag::kNewSubfileType).value_or(0) & 1U) != 0;
        const std::vector<std::uint64_t> bits = ifd.numbers(tiff::tag::kBitsPerSample, 4);
        const bool eight =
            !bits.empty() && std::ranges::all_of(bits, [](std::uint64_t b) { return b == 8; });
        const std::uint32_t width = narrow(ifd.number(tiff::tag::kImageWidth));
        const std::uint32_t height = narrow(ifd.number(tiff::tag::kImageLength));
        if (!tiffs || base != 0 || !reduced || !eight || width == 0 || height == 0 ||
            (photometric != kRgb && photometric != kYCbCr) ||
            (strips.empty() && !ifd.has(tiff::tag::kTileOffsets))) {
            return;
        }
        out_.previews.push_back({.offset = strips.empty() ? 0 : strips[0],
                                 .length = counts.empty() ? 0 : counts[0],
                                 .ifd = ifd,
                                 .width = width,
                                 .height = height});
    }

    // An IFD, its SubIFDs and theirs, in that order.
    void tree(const tiff::Container& container, const tiff::Ifd& top, std::uint64_t base,
              bool tiffs) {
        ifd(top, base, tiffs);
        for (const std::uint64_t sub : top.numbers(tiff::tag::kSubIfds, kMaxSubIfds)) {
            const std::optional<tiff::Ifd> child = container.ifd(sub);
            if (!child) {
                continue;
            }
            ifd(*child, base, tiffs);
            for (const std::uint64_t inner : child->numbers(tiff::tag::kSubIfds, kMaxSubIfds)) {
                if (const std::optional<tiff::Ifd> grandchild = container.ifd(inner)) {
                    ifd(*grandchild, base, tiffs);
                }
            }
        }
    }

    // The previews of the maker note of the EXIF IFD exif (in container, whose structure starts
    // at base in the file; its bytes in memory when exif_bytes is set, else read from the file).
    void maker_note(const tiff::Container& container, const tiff::Ifd& exif, std::uint64_t base,
                    std::string_view make, std::span<const std::uint8_t> exif_bytes = {}) {
        const tiff::Entry* entry = exif.find(kMakerNote);
        if (entry == nullptr || entry->is_inline || entry->count < 16) {
            return;
        }
        const std::uint64_t at = base + entry->offset;  // the maker note in the file
        // The note (from skip bytes on) as a source of its own, read where needed: it may hold a
        // preview of a few MB.
        const auto source_from = [&](std::uint64_t skip) -> std::optional<tiff::Source> {
            if (skip >= entry->count) {
                return std::nullopt;
            }
            if (exif_bytes.empty()) {
                return tiff::Source{file_, at + skip, entry->count - skip};
            }
            if (entry->offset + skip >= exif_bytes.size()) {
                return std::nullopt;
            }
            const std::span<const std::uint8_t> rest =
                exif_bytes.subspan(static_cast<std::size_t>(entry->offset + skip));
            return tiff::Source{rest.first(static_cast<std::size_t>(
                std::min<std::uint64_t>(rest.size(), entry->count - skip)))};
        };
        const std::optional<tiff::Source> note = source_from(0);
        if (!note) {
            return;
        }
        const std::vector<std::uint8_t> head = note->read(0, 16);
        if (starts_with(head, "OLYMPUS\0II") || starts_with(head, "OLYMPUS\0MM")) {
            olympus(*note, head[8] == 'I', 12, at);
        } else if (starts_with(head, std::string_view{"OM SYSTEM\0\0\0", 12})) {
            olympus(*note, head[12] == 'I', 16, at);
        } else if (starts_with(head, std::string_view{"Nikon\0\x02", 7})) {
            if (const std::optional<tiff::Source> inner = source_from(10)) {
                nikon(*inner, at + 10);
            }
        } else if (make.starts_with("SAMSUNG") && base == 0) {
            // A plain IFD whose offsets count from the file's TIFF header.
            if (const std::optional<tiff::Ifd> ifd = container.ifd(entry->offset)) {
                if (const auto preview = ifd->number(kSamsungPreviewIfd)) {
                    if (const std::optional<tiff::Ifd> previews = container.ifd(*preview)) {
                        this->ifd(*previews, base, false);
                    }
                }
            }
        }
    }

    // The previews in the EXIF of a JPEG at offset in the file, whose first bytes are head: the
    // EXIF thumbnail, the maker note's; its orientation when orientation is set.
    void jpeg_exif(std::uint64_t offset, std::span<const std::uint8_t> head, bool orientation) {
        const std::optional<detail::JpegExif> exif = detail::jpeg_exif(head);
        if (!exif) {
            return;
        }
        const std::uint64_t base = offset + exif->offset;
        const tiff::Source source{exif->tiff};
        const std::optional<tiff::Container> container = tiff::Container::open(source);
        if (!container) {
            return;
        }
        const std::vector<tiff::Ifd> chain = container->chain(4);
        for (const tiff::Ifd& ifd : chain) {
            this->ifd(ifd, base, false);
        }
        if (chain.empty()) {
            return;
        }
        if (orientation) {
            set_orientation(chain.front());
        }
        if (const auto exif_at = chain.front().number(kExifIfd)) {
            if (const std::optional<tiff::Ifd> exif_ifd = container->ifd(*exif_at)) {
                maker_note(*container, *exif_ifd, base, make_of(chain.front()), exif->tiff);
            }
        }
    }

    void set_orientation(const tiff::Ifd& ifd) {
        const std::uint64_t value = ifd.number(tiff::tag::kOrientation).value_or(1);
        out_.orientation = value >= 1 && value <= 8 ? static_cast<std::uint8_t>(value) : 1;
    }

    static std::string make_of(const tiff::Ifd& ifd) {
        const std::vector<std::uint8_t> bytes = ifd.bytes(kMake, 64);
        std::string make(bytes.begin(), bytes.end());
        make.resize(std::min(make.size(), make.find('\0')));
        return make;
    }

private:
    // Olympus / OM System: an IFD after a header of header bytes, offsets from the maker note's
    // start (at, in the file). Its own IFD holds a thumbnail; its CameraSettings the large
    // preview.
    void olympus(const tiff::Source& note, bool little, std::size_t header, std::uint64_t at) {
        const tiff::Container container = tiff::Container::headless(note, little, header);
        const std::vector<tiff::Ifd> chain = container.chain(1);
        if (chain.empty()) {
            return;
        }
        const tiff::Ifd& main = chain.front();
        if (const tiff::Entry* thumbnail = main.find(kOlympusThumbnail);
            thumbnail != nullptr && !thumbnail->is_inline) {
            jpeg(at + thumbnail->offset, thumbnail->count);
        }
        const std::optional<std::uint64_t> settings = main.number(kOlympusCameraSettings);
        const std::optional<tiff::Ifd> cs = settings ? container.ifd(*settings) : std::nullopt;
        if (cs && cs->number(kOlympusPreviewValid).value_or(0) == 1) {
            const std::uint64_t start = cs->number(kOlympusPreviewStart).value_or(0);
            const std::uint64_t length = cs->number(kOlympusPreviewLength).value_or(0);
            if (start > 0) {
                jpeg(at + start, length);
            }
        }
    }

    // Nikon: "Nikon\0" and a version, then a TIFF structure of its own from byte 10 (note, at in
    // the file); its PreviewIFD holds a JPEG.
    void nikon(const tiff::Source& note, std::uint64_t at) {
        const std::optional<tiff::Container> container = tiff::Container::open(note);
        if (!container) {
            return;
        }
        const std::vector<tiff::Ifd> chain = container->chain(1);
        if (chain.empty()) {
            return;
        }
        if (const auto preview = chain.front().number(kNikonPreviewIfd)) {
            if (const std::optional<tiff::Ifd> ifd = container->ifd(*preview)) {
                this->ifd(*ifd, at, false);
            }
        }
    }

    const detail::FdFile& file_;
    CameraFile& out_;
};

// The main image's IFD as exiv2 took it: the first of IFD0 and its SubIFDs marked full
// resolution (NewSubfileType 0), passing over those holding a JPEG; IFD0 when none is.
tiff::Ifd primary(const tiff::Container& container, const tiff::Ifd& first) {
    std::vector<tiff::Ifd> groups{first};
    for (const std::uint64_t sub : first.numbers(tiff::tag::kSubIfds, kPrimaryGroups)) {
        if (std::optional<tiff::Ifd> child = container.ifd(sub)) {
            groups.push_back(std::move(*child));
        }
    }
    tiff::Ifd chosen = first;
    for (const tiff::Ifd& group : groups) {
        const std::optional<std::uint64_t> type = group.number(tiff::tag::kNewSubfileType);
        if (type && *type == 0) {
            chosen = group;
            if (!group.has(kJpegOffset)) {
                break;
            }
        }
    }
    return chosen;
}

std::optional<CameraFile> read_jpeg(const detail::FdFile& file, bool previews) {
    CameraFile out;
    out.type = Type::jpeg;
    const std::vector<std::uint8_t> head = file.read(0, kExifHead);
    Reader reader{file, out};
    if (previews) {
        reader.jpeg_exif(0, head, true);
    }
    if (const std::optional<detail::JpegSize> size = detail::jpeg_size(head)) {
        out.full_width = size->width;
        out.full_height = size->height;
    }
    return out;
}

// RAF: a header (big-endian) with the camera's JPEG and the raw data's own header, whose record
// 0x100 is the full raw size (height, width).
std::optional<CameraFile> read_raf(const detail::FdFile& file, bool previews) {
    const std::vector<std::uint8_t> header = file.read_exactly(0, 100);
    if (header.empty()) {
        return std::nullopt;
    }
    CameraFile out;
    out.type = Type::raf;
    Reader reader{file, out};
    const std::uint32_t jpeg_offset = be32(header, 84);
    const std::uint32_t jpeg_length = be32(header, 88);
    if (previews) {
        reader.jpeg(jpeg_offset, jpeg_length);
        reader.jpeg_exif(jpeg_offset,
                         file.read(jpeg_offset, std::min<std::size_t>(jpeg_length, kExifHead)),
                         true);
    }
    const std::uint32_t records_at = be32(header, 92);
    const std::uint32_t records_length = be32(header, 96);
    const std::vector<std::uint8_t> records =
        file.read(records_at, std::min<std::size_t>(records_length, std::size_t{64} * 1024));
    if (records.size() >= 4) {
        const std::uint32_t count = be32(records, 0);
        std::size_t at = 4;
        for (std::uint32_t i = 0; i < count && at + 4 <= records.size(); ++i) {
            const std::uint16_t tag = be16(records, at);
            const std::uint16_t size = be16(records, at + 2);
            if (tag == kRafFullSize && size >= 4 && at + 8 <= records.size()) {
                out.full_height = be16(records, at + 4);
                out.full_width = be16(records, at + 6);
                break;
            }
            at += 4 + std::size_t{size};
        }
    }
    return out;
}

std::optional<CameraFile> read_tiff(const detail::FdFile& file, std::span<const std::uint8_t> head,
                                    bool previews) {
    const tiff::Source source{file};
    const std::optional<tiff::Container> container = tiff::Container::open(source, true);
    if (!container) {
        return std::nullopt;
    }
    const std::vector<tiff::Ifd> chain = container->chain(previews ? 16 : 1);
    if (chain.empty()) {
        return std::nullopt;
    }
    CameraFile out;
    if (head[2] == 0x55 && head[3] == 0) {
        out.type = Type::rw2;
    } else if (starts_with(head, "IIRO") || starts_with(head, "IIRS") ||
               starts_with(head, "MMOR")) {
        out.type = Type::orf;
    } else if (head.size() >= 10 && head[8] == 'C' && head[9] == 'R') {
        out.type = Type::cr2;
    }
    Reader reader{file, out};
    const tiff::Ifd& first = chain.front();
    // Canon's TIFF previews (CR2's third IFD) are sensor data without white balance or tone
    // curve: green and dark (checked on the 1D Mark II, 1D X and 5DS samples).
    const bool tiffs = out.type != Type::cr2;
    for (const tiff::Ifd& ifd : chain) {
        if (previews) {
            reader.tree(*container, ifd, 0, tiffs);
        }
    }
    if (out.type == Type::rw2 && previews) {
        // Panasonic's JPEG, whole, in IFD0's tag 0x2E; its own EXIF has the thumbnail.
        if (const tiff::Entry* entry = first.find(kPanasonicJpeg);
            entry != nullptr && !entry->is_inline) {
            reader.jpeg(entry->offset, entry->count);
            reader.jpeg_exif(
                entry->offset,
                file.read(entry->offset, std::min<std::size_t>(entry->count, kExifHead)), false);
        }
    }
    const std::optional<std::uint64_t> exif_at = first.number(kExifIfd);
    const std::optional<tiff::Ifd> exif = exif_at ? container->ifd(*exif_at) : std::nullopt;
    if (exif && previews) {
        reader.maker_note(*container, *exif, 0, Reader::make_of(first));
    }
    reader.set_orientation(first);
    switch (out.type) {
        case Type::cr2:
            if (exif) {
                out.full_width = narrow(exif->number(kPixelXDimension));
                out.full_height = narrow(exif->number(kPixelYDimension));
            }
            break;
        case Type::rw2:
            out.full_width = narrow(first.number(kPanasonicSensorWidth));
            out.full_height = narrow(first.number(kPanasonicSensorHeight));
            break;
        case Type::orf:
            out.full_width = narrow(first.number(tiff::tag::kImageWidth));
            out.full_height = narrow(first.number(tiff::tag::kImageLength));
            break;
        default: {
            const tiff::Ifd main = primary(*container, first);
            out.full_width = narrow(main.number(tiff::tag::kImageWidth));
            out.full_height = narrow(main.number(tiff::tag::kImageLength));
            break;
        }
    }
    return out;
}

}  // namespace

std::optional<CameraFile> read(const detail::FdFile& file, bool previews) {
    const std::vector<std::uint8_t> head = file.read(0, 16);
    if (head.size() < 16) {
        return std::nullopt;
    }
    if (head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) {
        return read_jpeg(file, previews);
    }
    if (starts_with(head, "FUJIFILMCCD-RAW ")) {
        return read_raf(file, previews);
    }
    return read_tiff(file, head, previews);
}

}  // namespace rummage::formats::image::camera
