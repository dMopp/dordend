#include <cstring>
#include <memory>
#include <webp/decode.h>
#include <webp/demux.h>

#include "decoders.hpp"

namespace rummage::formats::image::detail {
namespace {

struct DemuxDeleter {
    void operator()(WebPDemuxer* demux) const { WebPDemuxDelete(demux); }
};

struct AnimDecoderDeleter {
    void operator()(WebPAnimDecoder* decoder) const { WebPAnimDecoderDelete(decoder); }
};

// The orientation in the file's EXIF chunk, if it has one.
std::uint8_t orientation_of(const WebPData& data) {
    const std::unique_ptr<WebPDemuxer, DemuxDeleter> demux{WebPDemux(&data)};
    if (!demux || (WebPDemuxGetI(demux.get(), WEBP_FF_FORMAT_FLAGS) & EXIF_FLAG) == 0) {
        return 1;
    }
    WebPChunkIterator chunk{};
    std::uint8_t orientation = 1;
    if (WebPDemuxGetChunk(demux.get(), "EXIF", 1, &chunk) != 0) {
        orientation = exif_orientation({chunk.chunk.bytes, chunk.chunk.size});
    }
    WebPDemuxReleaseChunkIterator(&chunk);
    return orientation;
}

}  // namespace

void decode_webp(std::span<const std::uint8_t> data, const DecodeOptions& options,
                 Decoded& result) {
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(data.data(), data.size(), &features) != VP8_STATUS_OK ||
        features.width <= 0 || features.height <= 0) {
        result.error = "not a valid WebP file";
        return;
    }
    const auto width = static_cast<std::uint32_t>(features.width);
    const auto height = static_cast<std::uint32_t>(features.height);
    if (std::uint64_t{width} * height > options.max_pixels) {
        result.error = kTooLarge;
        return;
    }
    const WebPData webp{.bytes = data.data(), .size = data.size()};
    Image& image = result.image;
    image.width = width;
    image.height = height;
    image.rgba.resize(std::size_t{width} * height * 4);
    if (features.has_animation != 0) {
        // The first frame, composed on the canvas as the animation would show it.
        WebPAnimDecoderOptions anim_options{};
        WebPAnimDecoderOptionsInit(&anim_options);
        anim_options.color_mode = MODE_RGBA;
        anim_options.use_threads = 0;
        const std::unique_ptr<WebPAnimDecoder, AnimDecoderDeleter> decoder{
            WebPAnimDecoderNew(&webp, &anim_options)};
        std::uint8_t* frame = nullptr;
        int timestamp = 0;
        if (!decoder || WebPAnimDecoderGetNext(decoder.get(), &frame, &timestamp) == 0) {
            result.error = "cannot decode the first frame of the WebP animation";
            return;
        }
        std::memcpy(image.rgba.data(), frame, image.rgba.size());
    } else if (WebPDecodeRGBAInto(data.data(), data.size(), image.rgba.data(), image.rgba.size(),
                                  static_cast<int>(width * 4)) == nullptr) {
        result.error = "cannot decode the WebP image";
        return;
    }
    result.orientation = orientation_of(webp);
}

}  // namespace rummage::formats::image::detail
