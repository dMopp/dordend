// PDF parsing lives only in rummage-pdf, after the same cage as rummage-preview: MuPDF draws the
// first page. MuPDF carries its own fonts, so nothing has to be opened inside the cage.
#include "backend.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

namespace rummage::isolate::backend {
namespace {
constexpr std::int64_t kMaxPixels = 32'000'000;
constexpr std::uint32_t kMaxSide = 2048;
constexpr std::uint64_t kReadBudget = std::uint64_t{512} * 1024 * 1024;
constexpr std::size_t kStore = std::size_t{256} * 1024 * 1024;
constexpr std::size_t kChunk = 64 * 1024;
using Clock = std::chrono::steady_clock;

fz_context* context = nullptr;  // one for the helper's life: fonts and colour spaces stay loaded

// A file read through its descriptor with pread, bounded in bytes and time.
struct Source {
    int fd{-1};
    std::int64_t size{0};
    std::uint64_t read{0};
    Clock::time_point deadline{Clock::now() + std::chrono::seconds{7}};
    unsigned char buffer[kChunk]{};
};

int next(fz_context* ctx, fz_stream* stream, std::size_t) {
    auto& source = *static_cast<Source*>(stream->state);
    if (Clock::now() >= source.deadline || source.read >= kReadBudget) {
        fz_throw(ctx, FZ_ERROR_LIMIT, "pdf reading budget exhausted");
    }
    ssize_t got = 0;
    do {
        got = ::pread(source.fd, source.buffer, sizeof(source.buffer), stream->pos);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        fz_throw(ctx, FZ_ERROR_SYSTEM, "pdf read failed");
    }
    if (got == 0) {
        return EOF;
    }
    source.read += static_cast<std::uint64_t>(got);
    stream->rp = source.buffer;
    stream->wp = source.buffer + got;
    stream->pos += got;
    return *stream->rp++;
}

// fz_seek turns SEEK_CUR into SEEK_SET before it calls this.
void seek(fz_context* ctx, fz_stream* stream, std::int64_t offset, int whence) {
    auto& source = *static_cast<Source*>(stream->state);
    const std::int64_t base = whence == SEEK_END ? source.size : 0;
    if ((whence != SEEK_SET && whence != SEEK_END) || offset < -base ||
        offset > source.size - base) {
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "pdf seek out of range");
    }
    stream->pos = base + offset;
    stream->rp = stream->wp = source.buffer;
}

void keep(fz_context*, void*) {}  // the Source belongs to make_picture

void quiet(void*, const char*) {}

// What MuPDF made, before it becomes a picture::Loaded: plain values only, since fz_try jumps.
struct Drawn {
    fz_pixmap* pixmap{nullptr};
    picture::Failure failure{picture::Failure::none};
    const char* detail{""};
};

Drawn draw_first_page(fz_stream* stream, std::uint32_t width, std::uint32_t height) {
    fz_context* ctx = context;
    Drawn drawn;
    fz_document* document = nullptr;
    fz_page* page = nullptr;
    fz_var(document);
    fz_var(page);
    fz_var(drawn);
    fz_try(ctx) {
        document = fz_open_document_with_stream(ctx, "application/pdf", stream);
        if (fz_needs_password(ctx, document) != 0) {
            drawn.failure = picture::Failure::unreadable;
            drawn.detail = "pdf is password protected";
        } else if (fz_count_pages(ctx, document) < 1) {
            drawn.failure = picture::Failure::damaged;
            drawn.detail = "pdf without pages";
        } else {
            page = fz_load_page(ctx, document, 0);
            const fz_rect bounds = fz_bound_page(ctx, page);
            const float page_width = bounds.x1 - bounds.x0;
            const float page_height = bounds.y1 - bounds.y0;
            if (!std::isfinite(page_width) || !std::isfinite(page_height) || page_width < 1.0F ||
                page_height < 1.0F) {
                drawn.failure = picture::Failure::damaged;
                drawn.detail = "empty pdf page";
            } else {
                float scale = std::min(static_cast<float>(width) / page_width,
                                       static_cast<float>(height) / page_height);
                const double pixels =
                    static_cast<double>(page_width * scale) * (page_height * scale);
                if (pixels > static_cast<double>(kMaxPixels)) {
                    scale *=
                        static_cast<float>(std::sqrt(static_cast<double>(kMaxPixels) / pixels));
                }
                const fz_matrix ctm =
                    fz_concat(fz_translate(-bounds.x0, -bounds.y0), fz_scale(scale, scale));
                // White paper below the page: alpha 0 clears to white.
                drawn.pixmap = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0);
            }
        }
    }
    fz_always(ctx) {
        fz_drop_page(ctx, page);
        fz_drop_document(ctx, document);
    }
    fz_catch(ctx) {
        drawn.pixmap = nullptr;
        drawn.failure = fz_caught(ctx) == FZ_ERROR_LIMIT ? picture::Failure::unavailable
                                                         : picture::Failure::damaged;
        drawn.detail = drawn.failure == picture::Failure::unavailable
                           ? "pdf reading budget exhausted"
                           : "unreadable pdf";
    }
    return drawn;
}

picture::Loaded failure(picture::Failure reason, const char* detail) {
    picture::Loaded value;
    value.failure = reason;
    value.detail = detail;
    return value;
}

// A page drawn into RGB, as straight RGBA no larger than width x height.
picture::Image rgba_of(fz_pixmap* pixmap, std::uint32_t width, std::uint32_t height) {
    fz_context* ctx = context;
    const int n = fz_pixmap_components(ctx, pixmap);
    const int w = std::min(fz_pixmap_width(ctx, pixmap), static_cast<int>(width));
    const int h = std::min(fz_pixmap_height(ctx, pixmap), static_cast<int>(height));
    if (n != 3 || w <= 0 || h <= 0) {
        return {};
    }
    const std::ptrdiff_t stride = fz_pixmap_stride(ctx, pixmap);
    const unsigned char* samples = fz_pixmap_samples(ctx, pixmap);
    picture::Image image{.width = static_cast<std::uint32_t>(w),
                         .height = static_cast<std::uint32_t>(h),
                         .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(w) *
                                                           static_cast<std::size_t>(h) * 4)};
    std::uint8_t* out = image.rgba.data();
    for (int y = 0; y < h; ++y) {
        const unsigned char* row = samples + (y * stride);
        for (int x = 0; x < w; ++x) {
            *out++ = row[x * 3];
            *out++ = row[(x * 3) + 1];
            *out++ = row[(x * 3) + 2];
            *out++ = 255;
        }
    }
    return image;
}

// One page with text in the standard Helvetica, for the warm-up.
constexpr char kWarmUpPdf[] =
    "%PDF-1.4\n"
    "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
    "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
    "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 64 32]/Contents 4 0 R"
    "/Resources<</Font<</F1 5 0 R>>>>>>endobj\n"
    "4 0 obj<</Length 36>>stream\nBT /F1 12 Tf 4 10 Td (rummage) Tj ET\nendstream endobj\n"
    "5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n"
    "trailer<</Root 1 0 R>>\n%%EOF\n";
}  // namespace

void warm_up() {
    context = fz_new_context(nullptr, nullptr, kStore);
    if (context == nullptr) {
        return;
    }
    fz_set_warning_callback(context, quiet, nullptr);
    fz_set_error_callback(context, quiet, nullptr);
    fz_try(context) { fz_register_document_handlers(context); }
    fz_catch(context) {
        fz_drop_context(context);
        context = nullptr;
        return;
    }
    // Draw once before the cage closes: whatever MuPDF sets up on first use (the built-in font,
    // colour management) is set up now. The document is repaired, which is fine here.
    fz_stream* stream = nullptr;
    fz_var(stream);
    fz_try(context) {
        stream = fz_open_memory(context, reinterpret_cast<const unsigned char*>(kWarmUpPdf),
                                sizeof(kWarmUpPdf) - 1);
    }
    fz_catch(context) { stream = nullptr; }
    if (stream != nullptr) {
        const Drawn drawn = draw_first_page(stream, 64, 64);
        fz_drop_pixmap(context, drawn.pixmap);
        fz_drop_stream(context, stream);
    }
    fz_empty_store(context);
}

picture::Loaded make_picture(int fd, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return failure(picture::Failure::too_large, "empty pdf preview size");
    }
    if (context == nullptr) {
        return failure(picture::Failure::unavailable, "mupdf could not start");
    }
    width = std::min(width, kMaxSide);
    height = std::min(height, kMaxSide);
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return failure(picture::Failure::unreadable, "pdf input is not a regular file");
    }
    auto source = std::make_unique<Source>();
    source->fd = fd;
    source->size = st.st_size;
    fz_stream* stream = nullptr;
    fz_var(stream);
    fz_try(context) {
        stream = fz_new_stream(context, source.get(), next, keep);
        stream->seek = seek;
    }
    fz_catch(context) { stream = nullptr; }
    if (stream == nullptr) {
        return failure(picture::Failure::unavailable, "allocation failed");
    }
    const Drawn drawn = draw_first_page(stream, width, height);
    fz_drop_stream(context, stream);
    picture::Loaded loaded;
    if (drawn.pixmap == nullptr) {
        loaded = failure(
            drawn.failure == picture::Failure::none ? picture::Failure::damaged : drawn.failure,
            drawn.detail);
    } else {
        loaded.image = rgba_of(drawn.pixmap, width, height);
        fz_drop_pixmap(context, drawn.pixmap);
        if (loaded.image.empty()) {
            loaded = failure(picture::Failure::damaged, "unexpected pdf pixmap");
        }
    }
    // Pages share nothing worth keeping; give the store back between files.
    fz_empty_store(context);
    return loaded;
}

picture::PhotoInfo photo_info(int) { return {}; }

text::Loaded read_text(int, const text::Limits&) {
    text::Loaded value;
    value.failure = text::Failure::unavailable;
    return value;
}

void colour(text::Text&, std::string_view, std::string_view) {}
}  // namespace rummage::isolate::backend
