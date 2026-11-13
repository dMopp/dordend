#include "rummage/formats/text/read.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "rummage/formats/text/syntax.hpp"

namespace rummage::formats::text {
namespace {

using rummage::text::Encoding;
using rummage::text::Failure;
using rummage::text::Limits;
using rummage::text::Loaded;
using rummage::text::Text;

constexpr char32_t kReplacement = 0xFFFD;

// Windows-1252's 0x80-0x9F (the rest is Latin-1, the byte's own code point). The five bytes it
// leaves undefined are their C1 control code points, as the WHATWG Encoding Standard reads them.
constexpr std::array<char32_t, 32> kCp1252{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

void append_utf8(std::string& out, char32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
}

// How many of bytes are whole, valid UTF-8 characters (no overlong forms, surrogates or code
// points past U+10FFFF), the rest being one character cut at the end; nullopt when they are not
// UTF-8.
std::optional<std::size_t> utf8_prefix(std::span<const std::uint8_t> bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        // Eight ASCII bytes at a time: most text is mostly ASCII.
        while (i + 8 <= bytes.size()) {
            std::uint64_t word = 0;
            std::memcpy(&word, bytes.data() + i, sizeof(word));
            if ((word & 0x8080808080808080U) != 0) {
                break;
            }
            i += 8;
        }
        if (i >= bytes.size()) {
            break;
        }
        const std::uint8_t lead = bytes[i];
        if (lead < 0x80U) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t code = 0;
        if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
            code = lead & 0x1FU;
        } else if (lead >= 0xE0U && lead <= 0xEFU) {
            length = 3;
            code = lead & 0x0FU;
        } else if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
            code = lead & 0x07U;
        } else {
            return std::nullopt;
        }
        for (std::size_t k = 1; k < length; ++k) {
            if (i + k >= bytes.size()) {
                return i;  // cut at the end
            }
            const std::uint8_t next = bytes[i + k];
            if ((next & 0xC0U) != 0x80U) {
                return std::nullopt;
            }
            code = (code << 6U) | (next & 0x3FU);
        }
        if ((length == 3 && (code < 0x800 || (code >= 0xD800 && code <= 0xDFFF))) ||
            (length == 4 && (code < 0x10000 || code > 0x10FFFF))) {
            return std::nullopt;
        }
        i += length;
    }
    return bytes.size();
}

std::string from_cp1252(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() + (bytes.size() / 2));
    for (const std::uint8_t byte : bytes) {
        if (byte < 0x80U) {
            out.push_back(static_cast<char>(byte));
        } else if (byte < 0xA0U) {
            append_utf8(out, kCp1252.at(byte - 0x80U));
        } else {
            append_utf8(out, byte);
        }
    }
    return out;
}

// UTF-16 after its byte order mark, to UTF-8; nullopt when it holds U+0000. A lone surrogate is
// U+FFFD; so is an odd last byte, unless the file goes on (then it is dropped, as is a pair cut
// in two).
std::optional<std::string> from_utf16(std::span<const std::uint8_t> bytes, bool little, bool more) {
    const auto unit = [&](std::size_t at) -> char32_t {
        return little ? static_cast<char32_t>(bytes[at] | (bytes[at + 1] << 8U))
                      : static_cast<char32_t>((bytes[at] << 8U) | bytes[at + 1]);
    };
    std::string out;
    out.reserve(bytes.size() + (bytes.size() / 2));
    std::size_t i = 0;
    while (i + 1 < bytes.size()) {
        char32_t code = unit(i);
        i += 2;
        if (code >= 0xD800 && code <= 0xDBFF) {
            if (i + 1 < bytes.size()) {
                const char32_t low = unit(i);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    i += 2;
                    code = 0x10000 + ((code - 0xD800) << 10U) + (low - 0xDC00);
                } else {
                    code = kReplacement;
                }
            } else if (more) {
                i -= 2;
                break;
            } else {
                code = kReplacement;
            }
        } else if (code >= 0xDC00 && code <= 0xDFFF) {
            code = kReplacement;
        }
        if (code == 0) {
            return std::nullopt;
        }
        append_utf8(out, code);
    }
    if (i < bytes.size() && !more) {
        append_utf8(out, kReplacement);
    }
    return out;
}

bool continuation(char byte) { return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U; }

// line with tabs expanded to the next multiple of tab_width columns (one column per character),
// cut at max_line_bytes on a character boundary.
std::string expand(std::string_view line, const Limits& limits) {
    std::string out;
    out.reserve(std::min(line.size(), limits.max_line_bytes));
    std::size_t column = 0;
    for (const char byte : line) {
        if (byte == '\t') {
            const std::size_t spaces = limits.tab_width - (column % limits.tab_width);
            out.append(spaces, ' ');
            column += spaces;
        } else {
            out.push_back(byte);
            column += continuation(byte) ? 0U : 1U;
        }
        if (out.size() >= limits.max_line_bytes) {
            break;
        }
    }
    if (out.size() > limits.max_line_bytes) {
        out.resize(limits.max_line_bytes);
    }
    // Never end inside a character: a lead byte whose continuation bytes were cut goes too.
    std::size_t start = out.size();
    while (start > 0 && continuation(out[start - 1])) {
        --start;
    }
    if (start > 0) {
        const auto lead = static_cast<unsigned char>(out[start - 1]);
        std::size_t length = 1;
        if (lead >= 0xF0U) {
            length = 4;
        } else if (lead >= 0xE0U) {
            length = 3;
        } else if (lead >= 0xC0U) {
            length = 2;
        }
        if (out.size() - (start - 1) < length) {
            out.resize(start - 1);
        }
    }
    return out;
}

Loaded binary() { return {.failure = Failure::binary, .detail = {}, .text = {}}; }

Loaded unreadable(int error) {
    return {.failure = Failure::unreadable,
            .detail = std::error_code{error, std::generic_category()}.message(),
            .text = {}};
}

// Reads [offset, offset + data.size()) of fd into data; the bytes read, or -1 and errno.
ssize_t read_at(int fd, std::span<std::uint8_t> data, std::size_t offset) {
    std::size_t got = 0;
    while (got < data.size()) {
        const ssize_t read =
            ::pread(fd, data.data() + got, data.size() - got, static_cast<off_t>(offset + got));
        if (read < 0 && errno == EINTR) {
            continue;
        }
        if (read < 0) {
            return -1;
        }
        if (read == 0) {
            break;
        }
        got += static_cast<std::size_t>(read);
    }
    return static_cast<ssize_t>(got);
}

}  // namespace

Loaded read_tail(int fd, std::size_t size, const Limits& limits) {
    std::vector<std::uint8_t> data(limits.max_bytes);
    const ssize_t got = read_at(fd, data, size - limits.max_bytes);
    if (got < 0) {
        return unreadable(errno);
    }
    std::span<const std::uint8_t> bytes = std::span{data}.first(static_cast<std::size_t>(got));
    // The first line is cut: it starts after the first line end (a file of one long line keeps
    // its end, from a character's start).
    const auto newline = std::ranges::find(bytes, std::uint8_t{'\n'});
    if (newline != bytes.end()) {
        bytes = bytes.subspan(static_cast<std::size_t>(newline - bytes.begin()) + 1);
    } else {
        while (!bytes.empty() && (bytes.front() & 0xC0U) == 0x80U) {
            bytes = bytes.subspan(1);
        }
    }
    Loaded loaded = decode(bytes, false, limits);
    if (loaded.failure == Failure::none) {
        loaded.text.truncated = true;
        loaded.text.tail = true;
    }
    return loaded;
}

Loaded decode(std::span<const std::uint8_t> bytes, bool more, const Limits& limits) {
    Text text;
    text.truncated = more;
    std::string converted;
    std::string_view content;
    const auto starts_with = [&](std::initializer_list<std::uint8_t> mark) {
        return bytes.size() >= mark.size() && std::ranges::equal(bytes.first(mark.size()), mark);
    };
    if (starts_with({0xFF, 0xFE}) || starts_with({0xFE, 0xFF})) {
        const bool little = bytes[0] == 0xFF;
        std::optional<std::string> utf8 = from_utf16(bytes.subspan(2), little, more);
        if (!utf8) {
            return binary();
        }
        converted = std::move(*utf8);
        content = converted;
        text.encoding = little ? Encoding::utf16le : Encoding::utf16be;
    } else {
        if (!bytes.empty() && std::memchr(bytes.data(), 0, bytes.size()) != nullptr) {
            return binary();
        }
        if (starts_with({0xEF, 0xBB, 0xBF})) {
            bytes = bytes.subspan(3);
        }
        const std::optional<std::size_t> valid = utf8_prefix(bytes);
        if (valid) {
            content = {reinterpret_cast<const char*>(bytes.data()), *valid};  // NOLINT: bytes
            if (*valid < bytes.size() && !more) {
                // The file's last character is cut: shown as one that cannot be read.
                converted.assign(content);
                append_utf8(converted, kReplacement);
                content = converted;
            }
        } else {
            converted = from_cp1252(bytes);
            content = converted;
            text.encoding = Encoding::cp1252;
        }
    }
    if (more) {
        // The last line is incomplete: shown only when it is the only one.
        const std::size_t last = content.rfind('\n');
        if (last != std::string_view::npos) {
            content = content.substr(0, last + 1);
        }
    }
    while (!content.empty()) {
        if (limits.max_lines != 0 && text.lines.size() == limits.max_lines) {
            text.truncated = true;
            break;
        }
        const std::size_t end = content.find('\n');
        std::string_view line = content.substr(0, end);
        if (end != std::string_view::npos && line.ends_with('\r')) {
            line.remove_suffix(1);
            text.crlf = true;
        }
        text.lines.push_back(expand(line, limits));
        content = end == std::string_view::npos ? std::string_view{} : content.substr(end + 1);
    }
    return {.failure = Failure::none, .detail = {}, .text = std::move(text)};
}

Loaded read(int fd, const Limits& limits) {
    // A log's end: its last max_bytes, from the first line that starts in them.
    if (limits.tail) {
        struct stat info{};
        if (::fstat(fd, &info) != 0) {
            return unreadable(errno);
        }
        const auto size = static_cast<std::size_t>(std::max<off_t>(info.st_size, 0));
        if (size > limits.max_bytes) {
            return read_tail(fd, size, limits);
        }
    }
    // One byte more than the limit tells whether the file goes on.
    std::vector<std::uint8_t> data(limits.max_bytes + 1);
    const ssize_t read = read_at(fd, data, 0);
    if (read < 0) {
        return unreadable(errno);
    }
    const auto got = static_cast<std::size_t>(read);
    const bool more = got > limits.max_bytes;
    return decode(std::span{data}.first(std::min(got, limits.max_bytes)), more, limits);
}

Loaded load(const rummage::text::LoadRequest& request) {
    const int fd = ::open(request.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return unreadable(errno);
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        return unreadable(EINVAL);
    }
    Loaded loaded = read(fd, request.limits);
    ::close(fd);
    if (loaded.failure == Failure::none) {
        colour(loaded.text, request.path.filename().string(), request.mime);
    }
    return loaded;
}

}  // namespace rummage::formats::text
