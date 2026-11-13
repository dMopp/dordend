// Video parsing lives only in rummage-media, after the same cage as rummage-preview.
#include "backend.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace rummage::isolate::backend {
namespace {
constexpr std::int64_t kMaxPixels = 32'000'000;
constexpr std::uint64_t kReadBudget = 128U * 1024U * 1024U;
using Clock = std::chrono::steady_clock;

struct Source {
    int fd;
    std::int64_t size;
    std::int64_t position{0};
    std::uint64_t read{0};
    Clock::time_point deadline{Clock::now() + std::chrono::seconds{7}};
};

int read_bytes(void* opaque, std::uint8_t* buffer, int count) {
    if (count <= 0) {
        return AVERROR(EINVAL);
    }
    auto& source = *static_cast<Source*>(opaque);
    if (Clock::now() >= source.deadline || source.read >= kReadBudget) {
        return AVERROR_EXIT;
    }
    const auto wanted =
        std::min<std::uint64_t>(static_cast<unsigned>(count), kReadBudget - source.read);
    ssize_t got;
    do {
        got = ::pread(source.fd, buffer, wanted, source.position);
    } while (got < 0 && errno == EINTR);
    if (got <= 0) {
        return got == 0 ? AVERROR_EOF : AVERROR(errno);
    }
    source.position += got;
    source.read += static_cast<std::uint64_t>(got);
    return static_cast<int>(got);
}

std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
    auto& source = *static_cast<Source*>(opaque);
    if (whence == AVSEEK_SIZE) {
        return source.size;
    }
    whence &= ~AVSEEK_FORCE;
    const auto base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? source.position : source.size;
    if ((whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) || offset < -base ||
        offset > source.size - base) {
        return AVERROR(EINVAL);
    }
    source.position = base + offset;
    return source.position;
}

int interrupted(void* opaque) { return Clock::now() >= static_cast<Source*>(opaque)->deadline; }

int deny_open(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
    return AVERROR(EACCES);
}

struct Input {
    AVFormatContext* format{avformat_alloc_context()};
    AVIOContext* io{nullptr};

    ~Input() {
        avformat_close_input(&format);
        if (io != nullptr) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};

struct Decoder {
    AVCodecContext* context{nullptr};
    AVPacket* packet{av_packet_alloc()};
    AVFrame* frame{av_frame_alloc()};

    ~Decoder() {
        avcodec_free_context(&context);
        av_packet_free(&packet);
        av_frame_free(&frame);
    }
};

picture::Loaded failure(picture::Failure reason, const char* detail) {
    picture::Loaded value;
    value.failure = reason;
    value.detail = detail;
    return value;
}

bool dark(const picture::Image& image) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) {
        count += image.rgba[i] < 12 && image.rgba[i + 1] < 12 && image.rgba[i + 2] < 12;
    }
    return count * 100 >= (image.rgba.size() / 4) * 98;
}

picture::Image convert(const AVFrame& frame, AVStream& stream, std::uint32_t width,
                       std::uint32_t height) {
    if (frame.width <= 0 || frame.height <= 0 ||
        static_cast<std::int64_t>(frame.width) * frame.height > kMaxPixels) {
        return {};
    }
    std::uint8_t orientation = 1;
    const AVPacketSideData* matrix =
        av_packet_side_data_get(stream.codecpar->coded_side_data,
                                stream.codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (matrix != nullptr && matrix->size >= 9 * sizeof(std::int32_t)) {
        const double angle =
            av_display_rotation_get(reinterpret_cast<const std::int32_t*>(matrix->data));
        if (std::isfinite(angle)) {
            const int turn = (static_cast<int>(std::lround(-angle / 90.0)) % 4 + 4) % 4;
            constexpr std::array<std::uint8_t, 4> orientations{1, 6, 3, 8};
            orientation = orientations[static_cast<std::size_t>(turn)];
        }
    }
    if (orientation == 6 || orientation == 8) {
        std::swap(width, height);
    }
    AVRational sar = frame.sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) {
        sar = stream.sample_aspect_ratio;
    }
    double aspect = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
    aspect = std::clamp(aspect, 1.0 / 100.0, 100.0);
    const double display_width = frame.width * aspect;
    const double fit =
        std::min({1.0, width / display_width, static_cast<double>(height) / frame.height});
    const int w = std::max(1, static_cast<int>(std::floor(display_width * fit)));
    const int h = std::max(1, static_cast<int>(std::floor(frame.height * fit)));
    SwsContext* scale =
        sws_getContext(frame.width, frame.height, static_cast<AVPixelFormat>(frame.format), w, h,
                       AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (scale == nullptr) {
        return {};
    }
    picture::Image image{.width = static_cast<std::uint32_t>(w),
                         .height = static_cast<std::uint32_t>(h),
                         .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(w) *
                                                           static_cast<std::size_t>(h) * 4)};
    std::array<std::uint8_t*, 4> output{image.rgba.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{w * 4, 0, 0, 0};
    const int lines = sws_scale(scale, frame.data, frame.linesize, 0, frame.height, output.data(),
                                strides.data());
    sws_freeContext(scale);
    return lines == h ? picture::apply_orientation(std::move(image), orientation)
                      : picture::Image{};
}
}  // namespace

void warm_up() {
    av_log_set_level(AV_LOG_QUIET);
    // Cache CPU detection before filesystem access and affinity queries are forbidden.
    static_cast<void>(av_get_cpu_flags());
    av_cpu_force_count(1);
}

picture::Loaded make_picture(int fd, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return failure(picture::Failure::too_large, "empty video preview size");
    }
    width = std::min(width, 1024U);
    height = std::min(height, 1024U);
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return failure(picture::Failure::unreadable, "video input is not a regular file");
    }
    Source source{.fd = fd, .size = st.st_size};
    Input input;
    auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
    if (input.format == nullptr || buffer == nullptr) {
        av_free(buffer);
        return failure(picture::Failure::unavailable, "allocation failed");
    }
    input.io = avio_alloc_context(buffer, 32768, 0, &source, read_bytes, nullptr, seek);
    if (input.io == nullptr) {
        av_free(buffer);
        return failure(picture::Failure::unavailable, "allocation failed");
    }
    input.format->pb = input.io;
    input.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    input.format->io_open = deny_open;  // Containers must never open another path or URL.
    input.format->interrupt_callback = {.callback = interrupted, .opaque = &source};
    input.format->probesize = 1024 * 1024;
    input.format->max_analyze_duration = AV_TIME_BASE;
    AVDictionary* options = nullptr;
    av_dict_set(&options, "format_whitelist",
                "mov,matroska,webm,avi,mpeg,mpegts,ogg,flv,asf,mjpeg,yuv4mpegpipe", 0);
    const int opened = avformat_open_input(&input.format, nullptr, nullptr, &options);
    av_dict_free(&options);
    if (opened < 0) {
        return failure(picture::Failure::damaged, "unreadable video container");
    }
    // Keep stream discovery single-threaded too; no decoder may create threads inside the cage.
    std::vector<AVDictionary*> stream_options(input.format->nb_streams, nullptr);
    for (auto& option : stream_options) {
        av_dict_set(&option, "threads", "1", 0);
        av_dict_set(&option, "max_pixels", "32000000", 0);
    }
    const int probed = avformat_find_stream_info(input.format, stream_options.data());
    for (auto*& option : stream_options) {
        av_dict_free(&option);
    }
    if (probed < 0) {
        return failure(picture::Failure::damaged, "no video stream information");
    }
    AVStream* stream = nullptr;
    for (unsigned i = 0; i < input.format->nb_streams; ++i) {
        auto* candidate = input.format->streams[i];
        if (candidate->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            (candidate->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
            stream = candidate;
            break;
        }
    }
    if (stream == nullptr) {
        return failure(picture::Failure::unknown_format, "no video stream");
    }
    if (stream->time_base.num <= 0 || stream->time_base.den <= 0) {
        return failure(picture::Failure::damaged, "invalid video time base");
    }
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        return failure(picture::Failure::unavailable, "video codec unavailable");
    }
    Decoder decoder;
    decoder.context = avcodec_alloc_context3(codec);
    if (decoder.context == nullptr || decoder.packet == nullptr || decoder.frame == nullptr ||
        avcodec_parameters_to_context(decoder.context, stream->codecpar) < 0) {
        return failure(picture::Failure::unavailable, "could not allocate video decoder");
    }
    decoder.context->thread_count = 1;
    decoder.context->thread_type = 0;
    decoder.context->max_pixels = kMaxPixels;
    if (avcodec_open2(decoder.context, codec, nullptr) < 0) {
        return failure(picture::Failure::damaged, "could not open video decoder");
    }
    const auto duration = input.format->duration;
    const std::array<std::int64_t, 3> positions{duration > AV_TIME_BASE ? duration / 10 : 0,
                                                duration > AV_TIME_BASE ? duration / 3 : 0, 0};
    picture::Image first_dark;
    for (std::size_t attempt = 0; attempt < positions.size(); ++attempt) {
        if (attempt > 0 && positions[0] == 0) {
            break;
        }
        const auto start =
            input.format->start_time == AV_NOPTS_VALUE ? 0 : input.format->start_time;
        if (start > 0 && positions[attempt] > std::numeric_limits<std::int64_t>::max() - start) {
            continue;
        }
        const auto timestamp =
            av_rescale_q(positions[attempt] + start, AV_TIME_BASE_Q, stream->time_base);
        if (av_seek_frame(input.format, stream->index, timestamp, AVSEEK_FLAG_BACKWARD) < 0 &&
            positions[attempt] != 0) {
            continue;
        }
        avcodec_flush_buffers(decoder.context);
        bool flushing = false;
        for (unsigned packets = 0; packets < 1024 && Clock::now() < source.deadline; ++packets) {
            const int received = avcodec_receive_frame(decoder.context, decoder.frame);
            if (received == 0) {
                const auto pts = decoder.frame->best_effort_timestamp;
                if (positions[attempt] > 0 && pts != AV_NOPTS_VALUE && pts < timestamp) {
                    av_frame_unref(decoder.frame);
                    continue;
                }
                auto image = convert(*decoder.frame, *stream, width, height);
                av_frame_unref(decoder.frame);
                if (image.empty()) {
                    break;
                }
                if (!dark(image)) {
                    picture::Loaded loaded;
                    loaded.image = std::move(image);
                    return loaded;
                }
                if (first_dark.empty()) {
                    first_dark = std::move(image);
                }
                break;
            }
            if (flushing || (received != AVERROR(EAGAIN) && received != AVERROR_EOF)) {
                break;
            }
            av_packet_unref(decoder.packet);
            if (av_read_frame(input.format, decoder.packet) < 0) {
                static_cast<void>(avcodec_send_packet(decoder.context, nullptr));
                flushing = true;
            } else if (decoder.packet->stream_index == stream->index) {
                if (avcodec_send_packet(decoder.context, decoder.packet) < 0) {
                    break;
                }
            }
        }
    }
    if (!first_dark.empty()) {
        picture::Loaded loaded;
        loaded.image = std::move(first_dark);
        return loaded;
    }
    return failure(Clock::now() >= source.deadline || source.read >= kReadBudget
                       ? picture::Failure::unavailable
                       : picture::Failure::damaged,
                   "no video frame within the decoding budget");
}

picture::PhotoInfo photo_info(int) { return {}; }

text::Loaded read_text(int, const text::Limits&) {
    text::Loaded value;
    value.failure = text::Failure::unavailable;
    return value;
}

void colour(text::Text&, std::string_view, std::string_view) {}
}  // namespace rummage::isolate::backend
