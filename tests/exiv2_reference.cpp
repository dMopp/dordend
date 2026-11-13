// rummage's exiv2 code as it was before ROADMAP 2e (preview.cpp and photo.cpp of commit
// b27367a, with exiv2's input over a descriptor from fd_file): the reference the readers that
// replace it are compared with (exiv2_reference_test.cpp). Tests only: never linked into
// rummage or its helper. Kept as it was; only the names moved into this namespace.
#include "exiv2_reference.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <exiv2/exiv2.hpp>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>

#include "fd_file.hpp"
#include "jpeg_segments.hpp"
#include "rummage/formats/image/decode.hpp"
#include "tiff/decode.hpp"

// NOLINTBEGIN: kept as it was in rummage
namespace rummage::formats::image::reference {

// exiv2's input over a descriptor: reads with pread at its own position, maps the file when
// exiv2 asks for a mapping (inside the helper a file cut short while mapped kills only the
// helper). Read-only: every write fails.
class FdIo final : public Exiv2::BasicIo {
public:
    explicit FdIo(const detail::FdFile& file) : file_{file} {}

    ~FdIo() override;
    FdIo(const FdIo&) = delete;
    FdIo& operator=(const FdIo&) = delete;
    FdIo(FdIo&&) = delete;
    FdIo& operator=(FdIo&&) = delete;

    int open() override;
    int close() override;
    size_t write(const Exiv2::byte* data, size_t count) override;
    size_t write(Exiv2::BasicIo& source) override;
    int putb(Exiv2::byte data) override;
    Exiv2::DataBuf read(size_t count) override;
    size_t read(Exiv2::byte* buffer, size_t count) override;
    int getb() override;
    void transfer(Exiv2::BasicIo& source) override;
    int seek(int64_t offset, Position from) override;
    Exiv2::byte* mmap(bool writable) override;
    int munmap() override;
    [[nodiscard]] size_t tell() const override;
    [[nodiscard]] size_t size() const override;
    [[nodiscard]] bool isopen() const override;
    [[nodiscard]] int error() const override;
    [[nodiscard]] bool eof() const override;
    [[nodiscard]] const std::string& path() const noexcept override;

    void populateFakeData() override {}

private:
    detail::FdFile file_;
    std::uint64_t position_{0};
    bool eof_{false};
    int error_{0};
    void* mapping_{nullptr};
};

FdIo::~FdIo() { munmap(); }

int FdIo::open() {
    position_ = 0;
    eof_ = false;
    error_ = 0;
    return 0;
}

int FdIo::close() {
    munmap();
    return 0;
}

size_t FdIo::write(const Exiv2::byte* /*data*/, size_t /*count*/) { return 0; }

size_t FdIo::write(Exiv2::BasicIo& /*source*/) { return 0; }

int FdIo::putb(Exiv2::byte /*data*/) { return EOF; }

Exiv2::DataBuf FdIo::read(size_t count) {
    Exiv2::DataBuf buffer{count};
    const size_t got = read(buffer.data(), count);
    buffer.resize(got);
    return buffer;
}

size_t FdIo::read(Exiv2::byte* buffer, size_t count) {
    if (position_ >= file_.size()) {
        eof_ = true;
        return 0;
    }
    const std::optional<std::size_t> got = file_.read_into(position_, {buffer, count});
    if (!got) {
        error_ = 1;
        return 0;
    }
    position_ += *got;
    eof_ = *got < count;
    return *got;
}

int FdIo::getb() {
    Exiv2::byte byte = 0;
    return read(&byte, 1) == 1 ? byte : EOF;
}

void FdIo::transfer(Exiv2::BasicIo& /*source*/) { error_ = 1; }

int FdIo::seek(int64_t offset, Position from) {
    std::int64_t base = 0;
    if (from == cur) {
        base = static_cast<std::int64_t>(position_);
    } else if (from == end) {
        base = static_cast<std::int64_t>(file_.size());
    }
    const std::int64_t target = base + offset;
    if (target < 0) {
        return 1;
    }
    position_ = static_cast<std::uint64_t>(target);
    eof_ = position_ > file_.size();
    return 0;
}

Exiv2::byte* FdIo::mmap(bool writable) {
    if (writable || file_.size() == 0) {
        return nullptr;
    }
    if (mapping_ == nullptr) {
        void* mapped = ::mmap(nullptr, file_.size(), PROT_READ, MAP_PRIVATE, file_.fd(), 0);
        if (mapped == MAP_FAILED) {
            error_ = 1;
            return nullptr;
        }
        mapping_ = mapped;
    }
    return static_cast<Exiv2::byte*>(mapping_);
}

int FdIo::munmap() {
    if (mapping_ != nullptr) {
        ::munmap(mapping_, file_.size());
        mapping_ = nullptr;
    }
    return 0;
}

size_t FdIo::tell() const { return static_cast<size_t>(position_); }

size_t FdIo::size() const { return static_cast<size_t>(file_.size()); }

bool FdIo::isopen() const { return true; }

int FdIo::error() const { return error_; }

bool FdIo::eof() const { return eof_; }

const std::string& FdIo::path() const noexcept {
    static const std::string kName{"(descriptor)"};
    return kName;
}

namespace {

// Bytes read to find a JPEG's EXIF segment (at most 64 KB, after the few segments before it),
// to find a preview's size in its header, and to read a raw file's first IFD.
constexpr std::size_t kExifHead = std::size_t{256} * 1024;
constexpr std::size_t kPreviewHead = std::size_t{64} * 1024;
constexpr std::size_t kIfdHead = std::size_t{64} * 1024;

// Where a preview is and how big it is (as shown, after the crop); data read only for the one
// chosen.
struct Candidate {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::optional<std::size_t> property;  // exiv2's preview, by index; else read from the file:
    std::size_t offset{0};
    std::size_t length{0};
    bool tiff{false};
    std::optional<PreviewCrop> crop;

    [[nodiscard]] std::uint32_t long_side() const { return std::max(width, height); }
};

std::optional<std::int64_t> number(const Exiv2::ExifData& exif, const char* key) {
    const auto it = exif.findKey(Exiv2::ExifKey{key});
    if (it == exif.end() || it->count() == 0) {
        return std::nullopt;
    }
    return it->toInt64();
}

// The Olympus / OM System maker-note preview: its offset counts from the maker note, which
// counts from the TIFF data (at the start of a raw file, after "Exif\0\0" in a JPEG).
std::optional<Candidate> olympus_preview(const detail::FdFile& file, const Exiv2::Image& image) {
    const Exiv2::ExifData& exif = image.exifData();
    const auto valid = number(exif, "Exif.OlympusCs.PreviewImageValid");
    const auto start = number(exif, "Exif.OlympusCs.PreviewImageStart");
    const auto length = number(exif, "Exif.OlympusCs.PreviewImageLength");
    const auto maker_note = number(exif, "Exif.MakerNote.Offset");
    if (valid.value_or(0) != 1 || !start || !length || !maker_note || *start <= 0 || *length <= 4 ||
        *maker_note < 0) {
        return std::nullopt;
    }
    std::size_t tiff = 0;
    if (image.imageType() == Exiv2::ImageType::jpeg) {
        const std::vector<std::uint8_t> head = file.read(0, kExifHead);
        const std::optional<detail::JpegExif> segment = detail::jpeg_exif(head);
        if (!segment) {
            return std::nullopt;
        }
        tiff = segment->offset;
    }
    Candidate candidate;
    candidate.offset =
        tiff + static_cast<std::size_t>(*maker_note) + static_cast<std::size_t>(*start);
    candidate.length = static_cast<std::size_t>(*length);
    // Its size is in its own header: read the start of it.
    const std::vector<std::uint8_t> head =
        file.read_exactly(candidate.offset, std::min(candidate.length, kPreviewHead));
    const std::optional<detail::JpegSize> size = detail::jpeg_size(head);
    if (!size) {
        return std::nullopt;  // not a JPEG where the tags say
    }
    candidate.width = size->width;
    candidate.height = size->height;
    return candidate;
}

// The Panasonic crop: the GX8 stores a 4:3 preview whatever the aspect ratio set in the camera,
// and the frame shot in its first IFD, in sensor pixels (CropTop/Left/Bottom/Right, tags
// 0x2F-0x32, which exiv2 does not read), within the sensor's borders (tags 0x04-0x07).
struct SensorCrop {
    std::uint32_t left{0};
    std::uint32_t top{0};
    std::uint32_t right{0};
    std::uint32_t bottom{0};
    std::uint32_t border_left{0};
    std::uint32_t border_top{0};
    std::uint32_t border_right{0};
    std::uint32_t border_bottom{0};

    // The same frame in a preview of width x height, which shows everything within the borders.
    [[nodiscard]] PreviewCrop in(std::uint32_t width, std::uint32_t height) const {
        // (at - from) * size / (to - from), rounded.
        const auto scale = [](std::uint32_t at, std::uint32_t from, std::uint32_t to,
                              std::uint32_t size) {
            const std::uint64_t span = to - from;
            return static_cast<std::uint32_t>(((std::uint64_t{at - from} * size * 2) + span) /
                                              (span * 2));
        };
        const std::uint32_t x0 = scale(left, border_left, border_right, width);
        const std::uint32_t x1 = scale(right, border_left, border_right, width);
        const std::uint32_t y0 = scale(top, border_top, border_bottom, height);
        const std::uint32_t y1 = scale(bottom, border_top, border_bottom, height);
        return {.x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0};
    }
};

std::optional<SensorCrop> panasonic_crop(const detail::FdFile& file) {
    const std::vector<std::uint8_t> head = file.read(0, kIfdHead);
    // RW2: TIFF's structure with its own magic number.
    const tiff::Source source{head};
    const std::optional<tiff::Container> container = tiff::Container::open(source, true);
    const std::optional<tiff::Ifd> ifd =
        container ? container->ifd(container->first_ifd()) : std::nullopt;
    if (!ifd) {
        return std::nullopt;
    }
    const auto tag = [&](std::uint16_t number) -> std::optional<std::uint32_t> {
        const std::optional<std::uint64_t> value = ifd->number(number);
        if (!value || *value > UINT32_MAX) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(*value);
    };
    const auto top = tag(0x2F);
    const auto left = tag(0x30);
    const auto bottom = tag(0x31);
    const auto right = tag(0x32);
    const auto border_top = tag(0x04);
    const auto border_left = tag(0x05);
    const auto border_bottom = tag(0x06);
    const auto border_right = tag(0x07);
    if (!top || !left || !bottom || !right || !border_top || !border_left || !border_bottom ||
        !border_right) {
        return std::nullopt;  // no crop tags (GX7 and older store the preview cropped)
    }
    const SensorCrop crop{.left = *left,
                          .top = *top,
                          .right = *right,
                          .bottom = *bottom,
                          .border_left = *border_left,
                          .border_top = *border_top,
                          .border_right = *border_right,
                          .border_bottom = *border_bottom};
    const bool inside = crop.border_left <= crop.left && crop.left < crop.right &&
                        crop.right <= crop.border_right && crop.border_top <= crop.top &&
                        crop.top < crop.bottom && crop.bottom <= crop.border_bottom;
    const bool whole = crop.left == crop.border_left && crop.right == crop.border_right &&
                       crop.top == crop.border_top && crop.bottom == crop.border_bottom;
    if (!inside || whole) {
        return std::nullopt;
    }
    return crop;
}

// The order the candidates are tried in: JPEGs large enough, smallest first; then TIFFs large
// enough (raw RGB, large on disk, and only some can be read), smallest first; then, when none is
// large enough, the largest, a JPEG before a TIFF of the same size. Ties keep the file's order.
void order_by_preference(std::vector<Candidate>& candidates, std::uint32_t min_long_side) {
    const auto key = [min_long_side](const Candidate& c) {
        const std::uint32_t side = c.long_side();
        if (side >= min_long_side) {
            return std::tuple{c.tiff ? 1 : 0, std::int64_t{side}, 0};
        }
        return std::tuple{2, -std::int64_t{side}, c.tiff ? 1 : 0};
    };
    std::ranges::stable_sort(candidates, {}, key);
}

}  // namespace

void init_exiv2() {
    static std::once_flag once;
    std::call_once(once, [] {
        Exiv2::XmpParser::initialize();  // makes exiv2 safe to use from several threads
        Exiv2::LogMsg::setLevel(Exiv2::LogMsg::mute);
    });
}

EmbeddedPreview embedded_preview(int fd, std::uint32_t min_long_side) {
    init_exiv2();
    EmbeddedPreview result;
    const std::optional<detail::FdFile> file = detail::FdFile::of(fd);
    if (!file) {
        result.error = "cannot read the file";
        return result;
    }
    try {
        // With an input of its own, exiv2 answers a type it does not know with nothing, not an
        // exception.
        const Exiv2::Image::UniquePtr image =
            Exiv2::ImageFactory::open(std::make_unique<FdIo>(*file));
        if (!image) {
            result.error = "a file type exiv2 does not read";
            return result;
        }
        image->readMetadata();
        std::vector<Candidate> candidates;
        Exiv2::PreviewManager previews{*image};
        const Exiv2::PreviewPropertiesList properties = previews.getPreviewProperties();
        // Canon's TIFF previews (CR2's third IFD) are sensor data without white balance or
        // tone curve: green and dark (checked on the 1D Mark II, 1D X and 5DS samples).
        const bool tiffs = image->imageType() != Exiv2::ImageType::cr2;
        for (std::size_t i = 0; i < properties.size(); ++i) {
            const bool tiff = properties[i].mimeType_ == "image/tiff";
            if (properties[i].mimeType_ == "image/jpeg" || (tiff && tiffs)) {
                candidates.push_back({.width = static_cast<std::uint32_t>(properties[i].width_),
                                      .height = static_cast<std::uint32_t>(properties[i].height_),
                                      .property = i,
                                      .offset = 0,
                                      .length = properties[i].size_,
                                      .tiff = tiff,
                                      .crop = std::nullopt});
            }
        }
        if (std::optional<Candidate> olympus = olympus_preview(*file, *image)) {
            candidates.push_back(*olympus);
        }
        const std::optional<SensorCrop> sensor_crop =
            image->imageType() == Exiv2::ImageType::rw2 ? panasonic_crop(*file) : std::nullopt;
        if (sensor_crop) {
            for (Candidate& candidate : candidates) {
                candidate.crop = sensor_crop->in(candidate.width, candidate.height);
                candidate.width = candidate.crop->width;
                candidate.height = candidate.crop->height;
            }
        }
        // The EXIF thumbnail is 4:3 whatever the photo: a 3:2 or 16:9 one comes with black bars
        // (Canon, Nikon, Sony, Panasonic GX7 1:1). Previews shaped unlike the largest are dropped.
        const auto largest = std::ranges::max_element(
            candidates, {}, [](const Candidate& c) { return std::uint64_t{c.width} * c.height; });
        if (largest != candidates.end() && largest->width > 0 && largest->height > 0) {
            const std::uint64_t w = largest->width;
            const std::uint64_t h = largest->height;
            std::erase_if(candidates, [&](const Candidate& c) {
                // |c.width / c.height - w / h| > 2 % of w / h, in integers.
                const std::uint64_t a = std::uint64_t{c.width} * h;
                const std::uint64_t b = std::uint64_t{c.height} * w;
                return (a > b ? a - b : b - a) * 50 > b;
            });
        }
        // Tried in order; one that turns out broken (Samsung EX2F's 640x360 JPEG stops before
        // its end) is passed over for the next.
        order_by_preference(candidates, min_long_side);
        std::vector<std::uint8_t> data;
        std::optional<Candidate> picked;
        for (Candidate chosen : candidates) {
            if (chosen.property) {
                const Exiv2::PreviewImage preview =
                    previews.getPreviewImage(properties.at(*chosen.property));
                data.assign(preview.pData(), preview.pData() + preview.size());
            } else {
                data = file->read_exactly(chosen.offset, chosen.length);
            }
            if (chosen.tiff) {
                // A TIFF preview compressed as JPEG (DNG): the JPEG itself, read like any other.
                if (const auto inner = tiff::jpeg_in_tiff(data)) {
                    const std::optional<detail::JpegSize> size = detail::jpeg_size(*inner);
                    if (!size) {
                        continue;
                    }
                    data = std::vector<std::uint8_t>(inner->begin(), inner->end());
                    chosen.tiff = false;
                    if (!chosen.crop) {
                        chosen.width = size->width;
                        chosen.height = size->height;
                    }
                }
            }
            const bool usable = chosen.tiff ? tiff::Container::open(tiff::Source{data}).has_value()
                                            : detail::whole_jpeg(data);
            if (usable) {
                picked = chosen;
                break;
            }
        }
        if (!picked) {
            return result;
        }
        const Candidate& chosen = *picked;
        result.data = std::move(data);
        result.tiff = chosen.tiff;
        result.width = chosen.width;
        result.height = chosen.height;
        result.crop = chosen.crop;
        if (sensor_crop) {
            result.full_width = sensor_crop->right - sensor_crop->left;
            result.full_height = sensor_crop->bottom - sensor_crop->top;
        } else {
            result.full_width = image->pixelWidth();
            result.full_height = image->pixelHeight();
        }
        const auto orientation = number(image->exifData(), "Exif.Image.Orientation");
        if (orientation && *orientation >= 1 && *orientation <= 8) {
            result.orientation = static_cast<std::uint8_t>(*orientation);
        }
    } catch (const std::exception& error) {
        // exiv2's own errors, and the standard ones its checks throw (std::overflow_error from
        // its safe arithmetic, found by fuzzing): a file it cannot read, never a crash.
        result = {};
        result.error = error.what();
    }
    return result;
}

namespace {

// A value as exiv2 prints it (interpreted: "1/2000 s", "Spot"), trimmed; empty when absent.
std::string printed(const Exiv2::ExifData& exif, const char* key) {
    const auto it = exif.findKey(Exiv2::ExifKey{key});
    if (it == exif.end() || it->count() == 0) {
        return {};
    }
    std::string text = it->print(&exif);
    const std::size_t end = text.find_last_not_of(" \t");
    text.resize(end == std::string::npos ? 0 : end + 1);
    return text;
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

// The darktable-style sidecar's rating (Xmp.xmp.Rating): 1-5 stars, -1 rejected.
std::optional<int> sidecar_rating(int sidecar_fd) {
    const std::optional<detail::FdFile> file = detail::FdFile::of(sidecar_fd);
    if (!file) {
        return std::nullopt;
    }
    try {
        const Exiv2::Image::UniquePtr xmp =
            Exiv2::ImageFactory::open(std::make_unique<FdIo>(*file));
        if (!xmp) {
            return std::nullopt;
        }
        xmp->readMetadata();
        const auto it = xmp->xmpData().findKey(Exiv2::XmpKey{"Xmp.xmp.Rating"});
        if (it == xmp->xmpData().end() || it->count() == 0) {
            return std::nullopt;
        }
        const auto rating = static_cast<int>(it->toInt64());
        if (rating >= -1 && rating <= 5 && rating != 0) {
            return rating;
        }
    } catch (const std::exception&) {
        return std::nullopt;  // an unreadable sidecar has no rating
    }
    return std::nullopt;
}

}  // namespace

PhotoInfo read_photo_info(int fd, int sidecar_fd) {
    init_exiv2();
    PhotoInfo info;
    try {
        const std::optional<detail::FdFile> file = detail::FdFile::of(fd);
        const Exiv2::Image::UniquePtr image =
            file ? Exiv2::ImageFactory::open(std::make_unique<FdIo>(*file)) : nullptr;
        if (!image) {
            // A type exiv2 does not read: no EXIF, but the sidecar may still rate it.
            info.rating = sidecar_fd >= 0 ? sidecar_rating(sidecar_fd) : std::nullopt;
            return info;
        }
        image->readMetadata();
        const Exiv2::ExifData& exif = image->exifData();
        info.width = image->pixelWidth();
        info.height = image->pixelHeight();
        const auto orientation = exif.findKey(Exiv2::ExifKey{"Exif.Image.Orientation"});
        if (orientation != exif.end() && orientation->count() > 0 && orientation->toInt64() >= 5 &&
            orientation->toInt64() <= 8) {
            std::swap(info.width, info.height);
        }
        info.camera =
            camera_of(printed(exif, "Exif.Image.Make"), printed(exif, "Exif.Image.Model"));
        info.lens = printed(exif, "Exif.Photo.LensModel");
        info.exposure_time = printed(exif, "Exif.Photo.ExposureTime");
        info.aperture = printed(exif, "Exif.Photo.FNumber");
        info.iso = printed(exif, "Exif.Photo.ISOSpeedRatings");
        info.focal_length = printed(exif, "Exif.Photo.FocalLength");
        info.taken = date_of(printed(exif, "Exif.Photo.DateTimeOriginal"));
        info.program = printed(exif, "Exif.Photo.ExposureProgram");
        info.exposure_bias = printed(exif, "Exif.Photo.ExposureBiasValue");
        info.metering = printed(exif, "Exif.Photo.MeteringMode");
        info.flash = printed(exif, "Exif.Photo.Flash");
        info.white_balance = printed(exif, "Exif.Photo.WhiteBalance");
        info.software = printed(exif, "Exif.Image.Software");
    } catch (const std::exception&) {
        info = {};
    }
    info.rating = sidecar_fd >= 0 ? sidecar_rating(sidecar_fd) : std::nullopt;
    return info;
}

}  // namespace rummage::formats::image::reference

// NOLINTEND
