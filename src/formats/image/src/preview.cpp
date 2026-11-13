#include "rummage/formats/image/preview.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <system_error>
#include <tuple>
#include <unistd.h>
#include <utility>

#include "camera/previews.hpp"
#include "fd_file.hpp"
#include "jpeg_segments.hpp"
#include "rummage/formats/image/decode.hpp"
#include "tiff/decode.hpp"
#include "tiff/extract.hpp"

namespace rummage::formats::image {
namespace {

// Bytes read to find a preview's size in its header, and to read a raw file's first IFD.
constexpr std::size_t kLongPreviewHead = std::size_t{256} * 1024;
constexpr std::size_t kPreviewHead = std::size_t{64} * 1024;
constexpr std::size_t kIfdHead = std::size_t{64} * 1024;

// Where a preview is and how big it is (as shown, after the crop); data read only for the one
// chosen.
struct Candidate {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::optional<tiff::Ifd> ifd;  // an image of the file's TIFF structure; else a JPEG stream:
    std::uint64_t offset{0};
    std::uint64_t length{0};
    bool tiff{false};
    std::optional<PreviewCrop> crop;

    [[nodiscard]] std::uint32_t long_side() const { return std::max(width, height); }
};

// The Panasonic crop: the GX8 stores a 4:3 preview whatever the aspect ratio set in the camera,
// and the frame shot in its first IFD, in sensor pixels (CropTop/Left/Bottom/Right, tags
// 0x2F-0x32), within the sensor's borders (tags 0x04-0x07).
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

EmbeddedPreview embedded_preview(const std::filesystem::path& path, std::uint32_t min_long_side) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        EmbeddedPreview result;
        result.error = std::error_code{errno, std::generic_category()}.message();
        return result;
    }
    EmbeddedPreview result = embedded_preview(fd, min_long_side);
    ::close(fd);
    return result;
}

EmbeddedPreview embedded_preview(int fd, std::uint32_t min_long_side) {
    EmbeddedPreview result;
    const std::optional<detail::FdFile> file = detail::FdFile::of(fd);
    if (!file) {
        result.error = "cannot read the file";
        return result;
    }
    const std::optional<camera::CameraFile> camera = camera::read(*file);
    if (!camera) {
        return result;  // a type that keeps none rummage reads (PNG, Sigma X3F): none, no error
    }
    std::vector<Candidate> candidates;
    for (const camera::Found& found : camera->previews) {
        if (found.ifd) {
            candidates.push_back({.width = found.width,
                                  .height = found.height,
                                  .ifd = found.ifd,
                                  .offset = 0,
                                  .length = 0,
                                  .tiff = true,
                                  .crop = std::nullopt});
            continue;
        }
        // Its size is in its own header: read the start of it (further when the segments
        // before the frame fill the first read: Fujifilm's EXIF and FlashPix ones).
        std::optional<detail::JpegSize> size;
        for (const std::size_t limit : {kPreviewHead, kLongPreviewHead}) {
            const std::vector<std::uint8_t> head = file->read_exactly(
                found.offset,
                static_cast<std::size_t>(std::min<std::uint64_t>(found.length, limit)));
            size = detail::jpeg_size(head);
            if (size || found.length <= limit) {
                break;
            }
        }
        if (!size) {
            continue;  // not a JPEG where the tags say
        }
        candidates.push_back({.width = size->width,
                              .height = size->height,
                              .ifd = std::nullopt,
                              .offset = found.offset,
                              .length = found.length,
                              .tiff = false,
                              .crop = std::nullopt});
    }
    const std::optional<SensorCrop> sensor_crop =
        camera->type == camera::Type::rw2 ? panasonic_crop(*file) : std::nullopt;
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
    // Tried in order; one that turns out broken (Samsung EX2F's 640x360 JPEG stops before its
    // end) is passed over for the next.
    order_by_preference(candidates, min_long_side);
    std::vector<std::uint8_t> data;
    std::optional<Candidate> picked;
    for (Candidate chosen : candidates) {
        data = chosen.ifd
                   ? tiff::extract(*chosen.ifd)
                   : file->read_exactly(chosen.offset, static_cast<std::size_t>(chosen.length));
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
        result.full_width = camera->full_width;
        result.full_height = camera->full_height;
    }
    result.orientation = camera->orientation;
    return result;
}

Decoded decode_preview(const EmbeddedPreview& preview) {
    Decoded result;
    if (preview.tiff) {
        result = tiff::decode(tiff::Source{preview.data}, {.apply_orientation = false});
    } else {
        result = decode(preview.data, {.apply_orientation = false});
    }
    result.orientation = preview.orientation;
    if (!result.error.empty() || !preview.crop) {
        return result;
    }
    const PreviewCrop& crop = *preview.crop;
    const Image& whole = result.image;
    if (crop.width == 0 || crop.height == 0 || crop.x + crop.width > whole.width ||
        crop.y + crop.height > whole.height) {
        return result;  // not the preview the crop was made for: shown whole
    }
    Image cropped{.width = crop.width, .height = crop.height, .rgba = {}};
    cropped.rgba.resize(std::size_t{crop.width} * crop.height * 4);
    const std::size_t row = std::size_t{crop.width} * 4;
    for (std::uint32_t y = 0; y < crop.height; ++y) {
        const std::size_t from = ((std::size_t{crop.y + y} * whole.width) + crop.x) * 4;
        std::memcpy(cropped.rgba.data() + (y * row), whole.rgba.data() + from, row);
    }
    result.image = std::move(cropped);
    return result;
}

}  // namespace rummage::formats::image
