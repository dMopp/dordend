#pragma once

// TIFF's structure, read with every offset and size checked (ROADMAP 2c): classic TIFF and
// BigTIFF, Intel and Motorola byte order, from a file (pread, never the whole file) or from
// bytes in memory (an EXIF block, a raw file's preview). The camera raw formats built like TIFF
// (ORF, RW2) are read too when asked: same structure, another magic number. The decoder
// (decode.hpp), the DNG reader and the camera previews' reader build on this.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../fd_file.hpp"

namespace rummage::formats::image::tiff {

// Where the bytes come from: a file read piece by piece, or a block in memory.
class Source {
public:
    explicit Source(const detail::FdFile& file) : file_{&file}, size_{file.size()} {}

    // length bytes of the file from base on, as if they were the whole of it (a maker note's
    // own TIFF structure).
    Source(const detail::FdFile& file, std::uint64_t base, std::uint64_t length)
        : file_{&file},
          base_{base},
          size_{file.size() > base ? std::min(length, file.size() - base) : 0} {}

    explicit Source(std::span<const std::uint8_t> bytes) : bytes_{bytes}, size_{bytes.size()} {}

    [[nodiscard]] std::uint64_t size() const { return size_; }

    // Exactly length bytes from offset; empty when they are not all there.
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset, std::size_t length) const;
    // Exactly to.size() bytes from offset into to (a buffer kept between reads); false when they
    // are not all there.
    [[nodiscard]] bool read_into(std::uint64_t offset, std::span<std::uint8_t> to) const;

    [[nodiscard]] bool has(std::uint64_t offset, std::uint64_t length) const {
        return offset <= size_ && length <= size_ - offset;
    }

private:
    const detail::FdFile* file_{nullptr};
    std::uint64_t base_{0};
    std::span<const std::uint8_t> bytes_;
    std::uint64_t size_{0};
};

// The field types of TIFF 6.0 and BigTIFF.
enum class Type : std::uint16_t {  // NOLINT(performance-enum-size): the file's 16-bit value
    uint8 = 1,
    ascii = 2,
    uint16 = 3,
    uint32 = 4,
    rational = 5,
    sint8 = 6,
    undefined = 7,
    sint16 = 8,
    sint32 = 9,
    srational = 10,
    float32 = 11,
    float64 = 12,
    ifd = 13,
    uint64 = 16,
    sint64 = 17,
    ifd64 = 18,
};

// The bytes one value of a type takes; 0 for an unknown type.
[[nodiscard]] std::size_t type_size(std::uint16_t type) noexcept;

struct Entry {
    std::uint16_t tag{0};
    std::uint16_t type{0};
    std::uint64_t count{0};
    // The value's bytes when they fit in the entry (4 bytes, 8 in BigTIFF), else where they are.
    std::array<std::uint8_t, 8> inline_bytes{};
    std::uint64_t offset{0};
    bool is_inline{false};
};

class Ifd;

class Container {
public:
    // What magic numbers are accepted: 42 (TIFF) and 43 (BigTIFF) always; with any_magic, the
    // raw formats' own (RW2's 0x55, ORF's "RO"/"RS").
    [[nodiscard]] static std::optional<Container> open(const Source& source,
                                                       bool any_magic = false);

    // A TIFF structure without its header (a maker note's): byte order and first IFD given.
    [[nodiscard]] static Container headless(const Source& source, bool little,
                                            std::uint64_t first_ifd) {
        return Container{source, little, false, first_ifd};
    }

    [[nodiscard]] bool little_endian() const { return little_; }

    [[nodiscard]] bool big_tiff() const { return big_; }

    [[nodiscard]] std::uint64_t first_ifd() const { return first_; }

    [[nodiscard]] const Source& source() const { return source_; }

    // The IFD at offset; nullopt when it does not fit in the data or is absurdly long.
    [[nodiscard]] std::optional<Ifd> ifd(std::uint64_t offset) const;

    // The main chain from the first IFD, at most max IFDs, stopping at a loop or a broken one.
    [[nodiscard]] std::vector<Ifd> chain(std::size_t max = 64) const;

    // Integers in the data's byte order.
    [[nodiscard]] std::uint16_t u16(std::span<const std::uint8_t> bytes) const;
    [[nodiscard]] std::uint32_t u32(std::span<const std::uint8_t> bytes) const;
    [[nodiscard]] std::uint64_t u64(std::span<const std::uint8_t> bytes) const;

private:
    Container(const Source& source, bool little, bool big, std::uint64_t first)
        : source_{source}, little_{little}, big_{big}, first_{first} {}

    Source source_;
    bool little_;
    bool big_;
    std::uint64_t first_;
};

// One image file directory, its entries as found. Values are read on demand, through a copy of
// the container (a few words): an Ifd outlives the Container it came from.
class Ifd {
public:
    explicit Ifd(const Container& container) : container_{container} {}

    [[nodiscard]] const Entry* find(std::uint16_t tag) const;

    [[nodiscard]] bool has(std::uint16_t tag) const { return find(tag) != nullptr; }

    // The values of an integer tag (BYTE, SHORT, LONG, LONG8, IFD, IFD8; signed ones when not
    // negative), at most max_count of them; empty when missing, of another type, out of the
    // data or more than max_count.
    [[nodiscard]] std::vector<std::uint64_t> numbers(std::uint16_t tag,
                                                     std::size_t max_count = 1U << 24) const;
    // The first of them.
    [[nodiscard]] std::optional<std::uint64_t> number(std::uint16_t tag) const;
    // The raw bytes of a tag's values (any type), at most max_bytes; empty otherwise.
    [[nodiscard]] std::vector<std::uint8_t> bytes(std::uint16_t tag,
                                                  std::size_t max_bytes = 1U << 20) const;
    // The values of a numeric tag as reals (integers, RATIONAL, SRATIONAL, FLOAT, DOUBLE; a
    // fraction over zero is 0), at most max_count of them; empty when missing, of another type,
    // out of the data or more than max_count.
    [[nodiscard]] std::vector<double> reals(std::uint16_t tag,
                                            std::size_t max_count = 1U << 20) const;

    [[nodiscard]] const Container& container() const { return container_; }

    std::uint64_t offset{0};  // where it is
    std::uint64_t next{0};    // the next IFD's offset; 0 at the end of the chain
    std::vector<Entry> entries;

private:
    Container container_;
};

// Tags the readers use.
namespace tag {
inline constexpr std::uint16_t kNewSubfileType = 254;
inline constexpr std::uint16_t kImageWidth = 256;
inline constexpr std::uint16_t kImageLength = 257;
inline constexpr std::uint16_t kBitsPerSample = 258;
inline constexpr std::uint16_t kCompression = 259;
inline constexpr std::uint16_t kPhotometric = 262;
inline constexpr std::uint16_t kStripOffsets = 273;
inline constexpr std::uint16_t kOrientation = 274;
inline constexpr std::uint16_t kSamplesPerPixel = 277;
inline constexpr std::uint16_t kRowsPerStrip = 278;
inline constexpr std::uint16_t kStripByteCounts = 279;
inline constexpr std::uint16_t kPlanarConfiguration = 284;
inline constexpr std::uint16_t kPredictor = 317;
inline constexpr std::uint16_t kColorMap = 320;
inline constexpr std::uint16_t kTileWidth = 322;
inline constexpr std::uint16_t kTileLength = 323;
inline constexpr std::uint16_t kTileOffsets = 324;
inline constexpr std::uint16_t kTileByteCounts = 325;
inline constexpr std::uint16_t kSubIfds = 330;
inline constexpr std::uint16_t kExtraSamples = 338;
inline constexpr std::uint16_t kSampleFormat = 339;
inline constexpr std::uint16_t kJpegTables = 347;
inline constexpr std::uint16_t kCfaRepeatPatternDim = 33421;
inline constexpr std::uint16_t kCfaPattern = 33422;
inline constexpr std::uint16_t kDngVersion = 50706;
inline constexpr std::uint16_t kCfaPlaneColor = 50710;
inline constexpr std::uint16_t kCfaLayout = 50711;
inline constexpr std::uint16_t kLinearizationTable = 50712;
inline constexpr std::uint16_t kBlackLevelRepeatDim = 50713;
inline constexpr std::uint16_t kBlackLevel = 50714;
inline constexpr std::uint16_t kBlackLevelDeltaH = 50715;
inline constexpr std::uint16_t kBlackLevelDeltaV = 50716;
inline constexpr std::uint16_t kWhiteLevel = 50717;
inline constexpr std::uint16_t kDefaultCropOrigin = 50719;
inline constexpr std::uint16_t kDefaultCropSize = 50720;
inline constexpr std::uint16_t kColorMatrix1 = 50721;
inline constexpr std::uint16_t kColorMatrix2 = 50722;
inline constexpr std::uint16_t kCameraCalibration1 = 50723;
inline constexpr std::uint16_t kCameraCalibration2 = 50724;
inline constexpr std::uint16_t kAnalogBalance = 50727;
inline constexpr std::uint16_t kAsShotNeutral = 50728;
inline constexpr std::uint16_t kAsShotWhiteXy = 50729;
inline constexpr std::uint16_t kBaselineExposure = 50730;
inline constexpr std::uint16_t kCalibrationIlluminant1 = 50778;
inline constexpr std::uint16_t kCalibrationIlluminant2 = 50779;
inline constexpr std::uint16_t kActiveArea = 50829;
inline constexpr std::uint16_t kProfileToneCurve = 50940;
inline constexpr std::uint16_t kForwardMatrix1 = 50964;
inline constexpr std::uint16_t kForwardMatrix2 = 50965;
inline constexpr std::uint16_t kOpcodeList1 = 51008;
inline constexpr std::uint16_t kOpcodeList2 = 51009;
inline constexpr std::uint16_t kOpcodeList3 = 51022;
inline constexpr std::uint16_t kBaselineExposureOffset = 51109;
}  // namespace tag

}  // namespace rummage::formats::image::tiff
