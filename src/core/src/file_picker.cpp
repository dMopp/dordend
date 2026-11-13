#include "rummage/core/file_picker.hpp"

#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

namespace rummage::core {
namespace {
constexpr std::size_t kLimit = 4 * 1024 * 1024;
constexpr std::uint32_t kItems = 16384;

struct Writer {
    std::string bytes;

    void number(std::uint32_t value) {
        if (bytes.size() > kLimit - 4) {
            throw std::runtime_error{"Picker message too large"};
        }
        for (unsigned shift = 0; shift < 32; shift += 8) {
            bytes.push_back(static_cast<char>((value >> shift) & 255U));
        }
    }

    void string(const std::string& value) {
        if (value.size() > kLimit - 4 || bytes.size() > kLimit - 4 - value.size() ||
            value.find('\0') != std::string::npos) {
            throw std::runtime_error{"Invalid picker string"};
        }
        number(static_cast<std::uint32_t>(value.size()));
        bytes += value;
    }

    template <class T, class F>
    void list(const std::vector<T>& values, F write) {
        if (values.size() > kItems) {
            throw std::runtime_error{"Too many picker items"};
        }
        number(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) {
            write(value);
        }
    }

    void finish(int fd) const {
        if (bytes.size() > kLimit || ftruncate(fd, 0) < 0) {
            throw std::runtime_error{"Cannot write picker message"};
        }
        std::size_t at = 0;
        while (at < bytes.size()) {
            const auto n = pwrite(fd, bytes.data() + at, bytes.size() - at, static_cast<off_t>(at));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                throw std::runtime_error{"Cannot write picker message"};
            }
            at += static_cast<std::size_t>(n);
        }
    }
};

struct Reader {
    std::string bytes;
    std::size_t at{};

    explicit Reader(int fd) {
        struct stat st{};
        if (fstat(fd, &st) < 0 || st.st_size < 0 || st.st_size > static_cast<off_t>(kLimit)) {
            throw std::runtime_error{"Invalid picker message size"};
        }
        bytes.resize(static_cast<std::size_t>(st.st_size));
        std::size_t got = 0;
        while (got < bytes.size()) {
            const auto n =
                pread(fd, bytes.data() + got, bytes.size() - got, static_cast<off_t>(got));
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                throw std::runtime_error{"Truncated picker message"};
            }
            got += static_cast<std::size_t>(n);
        }
    }

    std::uint32_t number() {
        if (bytes.size() - at < 4) {
            throw std::runtime_error{"Truncated picker message"};
        }
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at++])) << shift;
        }
        return value;
    }

    bool boolean() {
        const auto value = number();
        if (value > 1) {
            throw std::runtime_error{"Invalid picker boolean"};
        }
        return value != 0;
    }

    std::string string() {
        const auto size = number();
        if (size > bytes.size() - at) {
            throw std::runtime_error{"Truncated picker string"};
        }
        std::string value = bytes.substr(at, size);
        at += size;
        if (value.find('\0') != std::string::npos) {
            throw std::runtime_error{"NUL in picker string"};
        }
        return value;
    }

    template <class T, class F>
    std::vector<T> list(F read) {
        const auto count = number();
        if (count > kItems) {
            throw std::runtime_error{"Too many picker items"};
        }
        std::vector<T> values;
        for (std::uint32_t i = 0; i < count; ++i) {
            values.push_back(read());
        }
        return values;
    }

    void finish() const {
        if (at != bytes.size()) {
            throw std::runtime_error{"Trailing picker data"};
        }
    }
};

constexpr std::uint32_t kRequest = 0x4e465001;
constexpr std::uint32_t kResult = 0x4e465101;
}  // namespace

void write_picker_request(int fd, const PickerRequest& r) {
    Writer w;
    w.number(kRequest);
    w.number(static_cast<std::uint32_t>(r.mode));
    w.number(r.multiple);
    w.number(r.directory);
    w.string(r.title);
    w.string(r.accept_label);
    w.string(r.folder);
    w.string(r.name);
    w.string(r.parent_window);
    w.number(r.modal);
    w.list(r.files, [&](const auto& s) { w.string(s); });
    w.list(r.filters, [&](const PickerFilter& f) {
        w.string(f.label);
        w.list(f.patterns, [&](const PickerPattern& p) {
            w.number(p.kind);
            w.string(p.value);
        });
    });
    w.number(r.filter);
    w.list(r.choices, [&](const PickerChoice& c) {
        w.string(c.id);
        w.string(c.label);
        w.list(c.options, [&](const PickerOption& o) {
            w.string(o.id);
            w.string(o.label);
        });
        w.string(c.selected);
    });
    w.finish(fd);
}

PickerRequest read_picker_request(int fd) {
    Reader r{fd};
    if (r.number() != kRequest) {
        throw std::runtime_error{"Unknown picker protocol"};
    }
    PickerRequest out;
    const auto mode = r.number();
    if (mode > 2) {
        throw std::runtime_error{"Invalid picker mode"};
    }
    out.mode = static_cast<PickerRequest::Mode>(mode);
    out.multiple = r.boolean();
    out.directory = r.boolean();
    out.title = r.string();
    out.accept_label = r.string();
    out.folder = r.string();
    out.name = r.string();
    out.parent_window = r.string();
    out.modal = r.boolean();
    out.files = r.list<std::string>([&] { return r.string(); });
    out.filters = r.list<PickerFilter>([&] {
        PickerFilter f;
        f.label = r.string();
        f.patterns = r.list<PickerPattern>([&] {
            const auto kind = r.number();
            if (kind > 1) {
                throw std::runtime_error{"Invalid picker filter"};
            }
            return PickerPattern{kind, r.string()};
        });
        return f;
    });
    out.filter = r.number();
    if ((!out.filters.empty() && out.filter >= out.filters.size()) ||
        (out.filters.empty() && out.filter != 0)) {
        throw std::runtime_error{"Invalid picker filter index"};
    }
    out.choices = r.list<PickerChoice>([&] {
        PickerChoice c;
        c.id = r.string();
        c.label = r.string();
        c.options = r.list<PickerOption>([&] { return PickerOption{r.string(), r.string()}; });
        c.selected = r.string();
        return c;
    });
    r.finish();
    return out;
}

void write_picker_result(int fd, const PickerResult& result) {
    Writer w;
    w.number(kResult);
    w.number(result.response);
    w.list(result.paths, [&](const auto& s) { w.string(s); });
    w.number(result.filter);
    w.list(result.choices, [&](const auto& s) { w.string(s); });
    w.finish(fd);
}

PickerResult read_picker_result(int fd) {
    Reader r{fd};
    if (r.number() != kResult) {
        throw std::runtime_error{"Unknown picker protocol"};
    }
    PickerResult out;
    out.response = r.number();
    if (out.response > 2) {
        throw std::runtime_error{"Invalid picker response"};
    }
    out.paths = r.list<std::string>([&] { return r.string(); });
    out.filter = r.number();
    out.choices = r.list<std::string>([&] { return r.string(); });
    r.finish();
    return out;
}
}  // namespace rummage::core
