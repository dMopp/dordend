#include "rummage/text/text.hpp"

#include <mutex>
#include <utility>

namespace rummage::text {
namespace {

std::mutex loader_mutex;
Loader current_loader;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace

std::string_view encoding_name(Encoding encoding) noexcept {
    switch (encoding) {
        case Encoding::utf8:
            return "UTF-8";
        case Encoding::utf16le:
            return "UTF-16 LE";
        case Encoding::utf16be:
            return "UTF-16 BE";
        case Encoding::cp1252:
            return "Latin-1";
    }
    return "UTF-8";
}

bool looks_minified(const Text& text) noexcept {
    if (text.lines.empty()) {
        return false;
    }
    std::size_t bytes = 0;
    for (const std::string& line : text.lines) {
        bytes += line.size();
    }
    return bytes / text.lines.size() > kMinifiedLineBytes;
}

void set_loader(Loader loader) {
    const std::scoped_lock lock{loader_mutex};
    current_loader = std::move(loader);
}

Loaded load_text(const LoadRequest& request) {
    Loader loader;
    {
        const std::scoped_lock lock{loader_mutex};
        loader = current_loader;
    }
    if (!loader) {
        return {.failure = Failure::unavailable, .detail = "no engine reads text", .text = {}};
    }
    return loader(request);
}

}  // namespace rummage::text
