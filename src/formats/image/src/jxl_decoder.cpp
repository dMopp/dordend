#include <jxl/decode.h>
#include <jxl/decode_cxx.h>

#include "decoders.hpp"

namespace rummage::formats::image::detail {

void decode_jxl(std::span<const std::uint8_t> data, const DecodeOptions& options, Decoded& result) {
    const JxlDecoderPtr decoder = JxlDecoderMake(nullptr);
    if (!decoder ||
        JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) !=
            JXL_DEC_SUCCESS ||
        // The pixels as stored, and the orientation reported: decode() turns them like the other
        // formats', when asked to.
        JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetInput(decoder.get(), data.data(), data.size()) != JXL_DEC_SUCCESS) {
        result.error = "cannot start the JPEG XL decoder";
        return;
    }
    JxlDecoderCloseInput(decoder.get());
    const JxlPixelFormat format{.num_channels = 4,
                                .data_type = JXL_TYPE_UINT8,
                                .endianness = JXL_NATIVE_ENDIAN,
                                .align = 0};
    Image& image = result.image;
    for (;;) {
        switch (JxlDecoderProcessInput(decoder.get())) {
            case JXL_DEC_BASIC_INFO: {
                JxlBasicInfo info{};
                if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS ||
                    info.xsize == 0 || info.ysize == 0) {
                    result.error = "not a valid JPEG XL file";
                    return;
                }
                if (std::uint64_t{info.xsize} * info.ysize > options.max_pixels) {
                    result.error = kTooLarge;
                    return;
                }
                image.width = info.xsize;
                image.height = info.ysize;
                result.orientation = static_cast<std::uint8_t>(info.orientation);
                break;
            }
            case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
                std::size_t size = 0;
                if (JxlDecoderImageOutBufferSize(decoder.get(), &format, &size) !=
                        JXL_DEC_SUCCESS ||
                    size != std::size_t{image.width} * image.height * 4) {
                    result.error = "unexpected JPEG XL buffer size";
                    return;
                }
                image.rgba.resize(size);
                if (JxlDecoderSetImageOutBuffer(decoder.get(), &format, image.rgba.data(), size) !=
                    JXL_DEC_SUCCESS) {
                    result.error = "cannot decode the JPEG XL image";
                    return;
                }
                break;
            }
            case JXL_DEC_FULL_IMAGE:  // the first frame is enough
            case JXL_DEC_SUCCESS:
                if (image.rgba.empty()) {
                    result.error = "the JPEG XL file has no image";
                }
                return;
            case JXL_DEC_NEED_MORE_INPUT:
                result.error = "the JPEG XL file is truncated";
                return;
            default:
                result.error = "cannot decode the JPEG XL image";
                return;
        }
    }
}

}  // namespace rummage::formats::image::detail
