#include "rummage/picture/wuffs.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "wuffs-v0.4.c"  // NOLINT(bugprone-suspicious-include): a header without WUFFS_IMPLEMENTATION
#include "wuffs_config.hpp"

namespace rummage::picture {
namespace {

// Decodes into the Image's own vector (no copy afterwards), as RGBA with straight alpha, and
// keeps the EXIF block on the way.
class Callbacks : public wuffs_aux::DecodeImageCallbacks {
public:
    Callbacks(Image& image, std::uint64_t max_pixels, std::vector<std::uint8_t>& exif)
        : image_{image}, max_pixels_{max_pixels}, exif_{exif} {}

    wuffs_base__pixel_format SelectPixfmt(const wuffs_base__image_config& /*config*/) override {
        return wuffs_base__make_pixel_format(WUFFS_BASE__PIXEL_FORMAT__RGBA_NONPREMUL);
    }

    std::string HandleMetadata(const wuffs_base__more_information& minfo,
                               wuffs_base__slice_u8 raw) override {
        if (minfo.metadata__fourcc() == WUFFS_BASE__FOURCC__EXIF) {
            exif_.assign(raw.ptr, raw.ptr + raw.len);
        }
        return {};
    }

    AllocPixbufResult AllocPixbuf(const wuffs_base__image_config& config,
                                  bool /*allow_uninitialized_memory*/) override {
        const std::uint32_t width = config.pixcfg.width();
        const std::uint32_t height = config.pixcfg.height();
        if (width == 0 || height == 0) {
            return AllocPixbufResult(std::string{"the image has no pixels"});
        }
        if (std::uint64_t{width} * height > max_pixels_) {
            return AllocPixbufResult(std::string{kTooLarge});
        }
        const std::uint64_t length = config.pixcfg.pixbuf_len();
        if (length != std::uint64_t{width} * height * 4) {
            return AllocPixbufResult(std::string{wuffs_aux::DecodeImage_UnsupportedPixelFormat});
        }
        image_.width = width;
        image_.height = height;
        image_.rgba.assign(length, 0);  // transparent where a GIF frame does not reach
        wuffs_base__pixel_buffer pixbuf;
        const wuffs_base__status status = pixbuf.set_from_slice(
            &config.pixcfg, wuffs_base__make_slice_u8(image_.rgba.data(), image_.rgba.size()));
        if (!status.is_ok()) {
            return AllocPixbufResult(std::string{status.message()});
        }
        return {wuffs_aux::MemOwner{nullptr, &free}, pixbuf};
    }

private:
    Image& image_;
    std::uint64_t max_pixels_;
    std::vector<std::uint8_t>& exif_;
};

}  // namespace

WuffsDecoded decode_wuffs(std::span<const std::uint8_t> data, std::uint64_t max_pixels) {
    WuffsDecoded result;
    Callbacks callbacks{result.image, max_pixels, result.exif};
    wuffs_aux::sync_io::MemoryInput input{data.data(), data.size()};
    const wuffs_aux::DecodeImageResult decoded = wuffs_aux::DecodeImage(
        callbacks, input, wuffs_aux::DecodeImageArgQuirks::DefaultValue(),
        wuffs_aux::DecodeImageArgFlags{wuffs_aux::DecodeImageArgFlags::REPORT_METADATA_EXIF},
        wuffs_aux::DecodeImageArgPixelBlend::DefaultValue(),
        wuffs_aux::DecodeImageArgBackgroundColor::DefaultValue(),
        wuffs_aux::DecodeImageArgMaxInclDimension{std::numeric_limits<std::uint16_t>::max()});
    if (!decoded.error_message.empty()) {
        result.error = decoded.error_message;
        result.image = {};
    }
    return result;
}

std::optional<Image> decode_png(std::span<const std::uint8_t> data, std::string& error) {
    constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data.size() < signature.size() ||
        !std::equal(signature.begin(), signature.end(), data.begin())) {
        error = "not a PNG";
        return std::nullopt;
    }
    // A thumbnail is at most 1024 px a side; anything far larger is not one of ours.
    WuffsDecoded decoded = decode_wuffs(data, std::uint64_t{4096} * 4096);
    if (!decoded.error.empty()) {
        error = std::move(decoded.error);
        return std::nullopt;
    }
    return std::move(decoded.image);
}

}  // namespace rummage::picture
