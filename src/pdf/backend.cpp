// PDF parsing lives only in rummage-pdf, after the same cage as rummage-preview: PDFium draws the
// first page. PDFium carries the standard fonts, and the system font provider is empty, so nothing
// has to be opened inside the cage. PDFium parses only PDF, has no JavaScript in this build (no
// V8), reads through pread bounded in bytes and time, and draws progressively so a page that takes
// too long is stopped at a deadline.
#include "backend.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fpdf_progressive.h>
#include <fpdf_sysfontinfo.h>
#include <fpdfview.h>
#include <limits>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace rummage::isolate::backend {
namespace {
constexpr std::int64_t kMaxPixels = 32'000'000;
constexpr std::uint32_t kMaxSide = 2048;
constexpr std::uint64_t kReadBudget = std::uint64_t{512} * 1024 * 1024;
using Clock = std::chrono::steady_clock;
constexpr std::chrono::seconds kDeadline{7};

bool started = false;

// A file read through its descriptor with pread, bounded in bytes and time.
struct Source {
    int fd{-1};
    std::uint64_t read{0};
    Clock::time_point deadline{Clock::now() + kDeadline};
    bool exhausted{false};
};

int get_block(void* param, unsigned long position, unsigned char* buffer, unsigned long size) {
    auto& source = *static_cast<Source*>(param);
    if (Clock::now() >= source.deadline || source.read + size > kReadBudget) {
        source.exhausted = true;
        return 0;
    }
    unsigned long done = 0;
    while (done < size) {
        const ssize_t got =
            ::pread(source.fd, buffer + done, size - done, static_cast<off_t>(position + done));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return 0;
        }
        done += static_cast<unsigned long>(got);
    }
    source.read += size;
    return 1;
}

// Drawing pauses here; past the deadline it is not resumed.
struct Pause : IFSDK_PAUSE {
    Clock::time_point deadline;
};

FPDF_BOOL need_to_pause(IFSDK_PAUSE* pause) {
    return Clock::now() >= static_cast<Pause*>(pause)->deadline ? 1 : 0;
}

// No system fonts: every lookup finds nothing, so PDFium uses its built-in fonts.
void enum_fonts(FPDF_SYSFONTINFO*, void*) {}

void* map_font(FPDF_SYSFONTINFO*, int, FPDF_BOOL, int, int, const char*, FPDF_BOOL*) {
    return nullptr;
}

void* get_font(FPDF_SYSFONTINFO*, const char*) { return nullptr; }

unsigned long get_font_data(FPDF_SYSFONTINFO*, void*, unsigned int, unsigned char*, unsigned long) {
    return 0;
}

unsigned long get_face_name(FPDF_SYSFONTINFO*, void*, char*, unsigned long) { return 0; }

int get_font_charset(FPDF_SYSFONTINFO*, void*) { return 0; }

void delete_font(FPDF_SYSFONTINFO*, void*) {}

void release(FPDF_SYSFONTINFO*) {}

FPDF_SYSFONTINFO no_system_fonts{1,          release,       enum_fonts,    map_font,
                                 get_font,   get_font_data, get_face_name, get_font_charset,
                                 delete_font};

picture::Loaded failure(picture::Failure reason, const char* detail) {
    picture::Loaded value;
    value.failure = reason;
    value.detail = detail;
    return value;
}

struct Document {
    FPDF_DOCUMENT handle{nullptr};

    explicit Document(FPDF_DOCUMENT document) : handle(document) {}

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    ~Document() {
        if (handle != nullptr) {
            FPDF_CloseDocument(handle);
        }
    }
};

struct Page {
    FPDF_PAGE handle{nullptr};

    explicit Page(FPDF_PAGE page) : handle(page) {}

    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    ~Page() {
        if (handle != nullptr) {
            FPDF_ClosePage(handle);
        }
    }
};

struct Bitmap {
    FPDF_BITMAP handle{nullptr};

    explicit Bitmap(FPDF_BITMAP bitmap) : handle(bitmap) {}

    Bitmap(const Bitmap&) = delete;
    Bitmap& operator=(const Bitmap&) = delete;

    ~Bitmap() {
        if (handle != nullptr) {
            FPDFBitmap_Destroy(handle);
        }
    }
};

picture::Loaded draw(Source& source, std::int64_t size, std::uint32_t width, std::uint32_t height) {
    FPDF_FILEACCESS access{};
    access.m_FileLen = static_cast<unsigned long>(size);
    access.m_GetBlock = get_block;
    access.m_Param = &source;
    const Document document{FPDF_LoadCustomDocument(&access, nullptr)};
    if (document.handle == nullptr) {
        const unsigned long error = FPDF_GetLastError();
        if (error == FPDF_ERR_PASSWORD) {
            return failure(picture::Failure::locked, "pdf is password protected");
        }
        if (source.exhausted) {
            return failure(picture::Failure::unavailable, "pdf reading budget exhausted");
        }
        return failure(picture::Failure::damaged, "unreadable pdf");
    }
    if (FPDF_GetPageCount(document.handle) < 1) {
        return failure(picture::Failure::damaged, "pdf without pages");
    }
    const Page page{FPDF_LoadPage(document.handle, 0)};
    if (page.handle == nullptr) {
        return failure(source.exhausted ? picture::Failure::unavailable : picture::Failure::damaged,
                       "unreadable pdf page");
    }
    // The size as shown, with the page's own rotation.
    const double page_width = FPDF_GetPageWidthF(page.handle);
    const double page_height = FPDF_GetPageHeightF(page.handle);
    if (!std::isfinite(page_width) || !std::isfinite(page_height) || page_width < 1.0 ||
        page_height < 1.0) {
        return failure(picture::Failure::damaged, "empty pdf page");
    }
    double scale = std::min(static_cast<double>(width) / page_width,
                            static_cast<double>(height) / page_height);
    const double pixels = page_width * scale * page_height * scale;
    if (pixels > static_cast<double>(kMaxPixels)) {
        scale *= std::sqrt(static_cast<double>(kMaxPixels) / pixels);
    }
    const auto w = static_cast<std::uint32_t>(
        std::clamp(std::lround(page_width * scale), 1L, static_cast<long>(width)));
    const auto h = static_cast<std::uint32_t>(
        std::clamp(std::lround(page_height * scale), 1L, static_cast<long>(height)));

    picture::Image image{.width = w,
                         .height = h,
                         .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
    // PDFium draws straight into the image: BGRx with reversed byte order is RGBx.
    const Bitmap bitmap{FPDFBitmap_CreateEx(static_cast<int>(w), static_cast<int>(h),
                                            FPDFBitmap_BGRx, image.rgba.data(),
                                            static_cast<int>(w * 4))};
    if (bitmap.handle == nullptr) {
        return failure(picture::Failure::too_large, "pdf bitmap could not be made");
    }
    FPDFBitmap_FillRect(bitmap.handle, 0, 0, static_cast<int>(w), static_cast<int>(h),
                        0xFFFFFFFF);  // white paper
    Pause pause{};
    pause.version = 1;
    pause.NeedToPauseNow = need_to_pause;
    pause.deadline = source.deadline;
    int status = FPDF_RenderPageBitmap_Start(bitmap.handle, page.handle, 0, 0, static_cast<int>(w),
                                             static_cast<int>(h), 0,
                                             FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER, &pause);
    while (status == FPDF_RENDER_TOBECONTINUED && Clock::now() < pause.deadline) {
        status = FPDF_RenderPage_Continue(page.handle, &pause);
    }
    FPDF_RenderPage_Close(page.handle);
    if (status != FPDF_RENDER_DONE) {
        return failure(status == FPDF_RENDER_TOBECONTINUED ? picture::Failure::unavailable
                                                           : picture::Failure::damaged,
                       status == FPDF_RENDER_TOBECONTINUED ? "pdf drawing took too long"
                                                           : "pdf drawing failed");
    }
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
        image.rgba[i] = 255;
    }
    picture::Loaded loaded;
    loaded.image = std::move(image);
    return loaded;
}

// One page with text in the standard Helvetica, for the warm-up.
constexpr std::string_view kWarmUpPdf =
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
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    FPDF_SetSystemFontInfo(&no_system_fonts);
    started = true;
    // Draw once before the cage closes: whatever PDFium sets up on first use (the built-in font,
    // colour management) is set up now.
    const Document document{FPDF_LoadMemDocument64(kWarmUpPdf.data(), kWarmUpPdf.size(), nullptr)};
    if (document.handle != nullptr) {
        const Page page{FPDF_LoadPage(document.handle, 0)};
        if (page.handle != nullptr) {
            const Bitmap bitmap{FPDFBitmap_Create(64, 32, 0)};
            if (bitmap.handle != nullptr) {
                FPDF_RenderPageBitmap(bitmap.handle, page.handle, 0, 0, 64, 32, 0, FPDF_ANNOT);
            }
        }
    }
}

picture::Loaded make_picture(int fd, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return failure(picture::Failure::too_large, "empty pdf preview size");
    }
    if (!started) {
        return failure(picture::Failure::unavailable, "pdfium could not start");
    }
    width = std::min(width, kMaxSide);
    height = std::min(height, kMaxSide);
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return failure(picture::Failure::unreadable, "pdf input is not a regular file");
    }
    if (static_cast<std::uint64_t>(st.st_size) > std::numeric_limits<unsigned long>::max()) {
        return failure(picture::Failure::too_large, "pdf too large");
    }
    Source source;
    source.fd = fd;
    return draw(source, st.st_size, width, height);
}

picture::PhotoInfo photo_info(int) { return {}; }

text::Loaded read_text(int, const text::Limits&) {
    text::Loaded value;
    value.failure = text::Failure::unavailable;
    return value;
}

void colour(text::Text&, std::string_view, std::string_view) {}
}  // namespace rummage::isolate::backend
