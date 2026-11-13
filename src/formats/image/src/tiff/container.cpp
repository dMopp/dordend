#include "container.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace rummage::formats::image::tiff {
namespace {

// An IFD with more entries than this is not one (TIFF files have tens).
constexpr std::uint64_t kMaxEntries = 4096;

}  // namespace

std::vector<std::uint8_t> Source::read(std::uint64_t offset, std::size_t length) const {
    if (!has(offset, length)) {
        return {};
    }
    if (file_ != nullptr) {
        return file_->read_exactly(base_ + offset, length);
    }
    const auto start = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    return {start, start + static_cast<std::ptrdiff_t>(length)};
}

bool Source::read_into(std::uint64_t offset, std::span<std::uint8_t> to) const {
    if (!has(offset, to.size())) {
        return false;
    }
    if (file_ != nullptr) {
        const std::optional<std::size_t> got = file_->read_into(base_ + offset, to);
        return got && *got == to.size();
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), to.size(), to.begin());
    return true;
}

std::size_t type_size(std::uint16_t type) noexcept {
    switch (static_cast<Type>(type)) {
        case Type::uint8:
        case Type::ascii:
        case Type::sint8:
        case Type::undefined:
            return 1;
        case Type::uint16:
        case Type::sint16:
            return 2;
        case Type::uint32:
        case Type::sint32:
        case Type::float32:
        case Type::ifd:
            return 4;
        case Type::rational:
        case Type::srational:
        case Type::float64:
        case Type::uint64:
        case Type::sint64:
        case Type::ifd64:
            return 8;
    }
    return 0;
}

std::uint16_t Container::u16(std::span<const std::uint8_t> b) const {
    return static_cast<std::uint16_t>(little_ ? b[0] | (b[1] << 8) : (b[0] << 8) | b[1]);
}

std::uint32_t Container::u32(std::span<const std::uint8_t> b) const {
    const std::uint32_t low = u16(b.subspan(little_ ? 0 : 2, 2));
    const std::uint32_t high = u16(b.subspan(little_ ? 2 : 0, 2));
    return (high << 16) | low;
}

std::uint64_t Container::u64(std::span<const std::uint8_t> b) const {
    const std::uint64_t low = u32(b.subspan(little_ ? 0 : 4, 4));
    const std::uint64_t high = u32(b.subspan(little_ ? 4 : 0, 4));
    return (high << 32) | low;
}

std::optional<Container> Container::open(const Source& source, bool any_magic) {
    const std::vector<std::uint8_t> head =
        source.read(0, std::min<std::uint64_t>(16, source.size()));
    if (head.size() < 8) {
        return std::nullopt;
    }
    bool little = false;
    if (head[0] == 'I' && head[1] == 'I') {
        little = true;
    } else if (head[0] != 'M' || head[1] != 'M') {
        return std::nullopt;
    }
    const Container probe{source, little, false, 0};
    const std::uint16_t magic = probe.u16(std::span{head}.subspan(2, 2));
    if (magic == 43) {
        // BigTIFF: offsets of 8 bytes, the header says so twice.
        if (head.size() < 16 || probe.u16(std::span{head}.subspan(4, 2)) != 8 ||
            probe.u16(std::span{head}.subspan(6, 2)) != 0) {
            return std::nullopt;
        }
        return Container{source, little, true, probe.u64(std::span{head}.subspan(8, 8))};
    }
    if (magic != 42 && !any_magic) {
        return std::nullopt;
    }
    return Container{source, little, false, probe.u32(std::span{head}.subspan(4, 4))};
}

std::optional<Ifd> Container::ifd(std::uint64_t offset) const {
    const std::size_t count_size = big_ ? 8 : 2;
    const std::size_t entry_size = big_ ? 20 : 12;
    const std::size_t offset_size = big_ ? 8 : 4;
    const std::vector<std::uint8_t> count_bytes = source_.read(offset, count_size);
    if (count_bytes.empty()) {
        return std::nullopt;
    }
    const std::uint64_t count = big_ ? u64(count_bytes) : u16(count_bytes);
    if (count == 0 || count > kMaxEntries) {
        return std::nullopt;
    }
    const std::size_t table = static_cast<std::size_t>(count) * entry_size;
    const std::vector<std::uint8_t> bytes = source_.read(offset + count_size, table);
    if (bytes.empty()) {
        return std::nullopt;
    }
    Ifd result{*this};
    result.offset = offset;
    result.entries.reserve(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const std::span<const std::uint8_t> raw =
            std::span{bytes}.subspan(i * entry_size, entry_size);
        Entry entry;
        entry.tag = u16(raw.subspan(0, 2));
        entry.type = u16(raw.subspan(2, 2));
        entry.count = big_ ? u64(raw.subspan(4, 8)) : u32(raw.subspan(4, 4));
        const std::span<const std::uint8_t> value = raw.subspan(big_ ? 12 : 8, offset_size);
        const std::size_t size = type_size(entry.type);
        // Inline when the values fit in the entry's own bytes; sizes that overflow are not.
        entry.is_inline =
            size != 0 && entry.count <= offset_size / size && entry.count * size <= offset_size;
        std::ranges::copy(value, entry.inline_bytes.begin());
        entry.offset = big_ ? u64(value) : u32(value);
        result.entries.push_back(entry);
    }
    // The next IFD's offset; a block cut right after the entries (small EXIF blocks are) ends
    // the chain there.
    const std::vector<std::uint8_t> next = source_.read(offset + count_size + table, offset_size);
    if (!next.empty()) {
        result.next = big_ ? u64(next) : u32(next);
    }
    return result;
}

std::vector<Ifd> Container::chain(std::size_t max) const {
    std::vector<Ifd> out;
    std::set<std::uint64_t> seen;
    std::uint64_t at = first_;
    while (at != 0 && out.size() < max && seen.insert(at).second) {
        std::optional<Ifd> found = ifd(at);
        if (!found) {
            break;
        }
        at = found->next;
        out.push_back(std::move(*found));
    }
    return out;
}

const Entry* Ifd::find(std::uint16_t tag) const {
    const auto it = std::ranges::find(entries, tag, &Entry::tag);
    return it == entries.end() ? nullptr : &*it;
}

std::vector<std::uint8_t> Ifd::bytes(std::uint16_t tag, std::size_t max_bytes) const {
    const Entry* entry = find(tag);
    if (entry == nullptr) {
        return {};
    }
    const std::size_t size = type_size(entry->type);
    if (size == 0 || entry->count > max_bytes / size) {
        return {};
    }
    const auto total = static_cast<std::size_t>(entry->count) * size;
    if (entry->is_inline) {
        return {entry->inline_bytes.begin(),
                entry->inline_bytes.begin() + static_cast<std::ptrdiff_t>(total)};
    }
    return container_.source().read(entry->offset, total);
}

namespace {

bool is_integer(Type type) {
    return type == Type::uint8 || type == Type::uint16 || type == Type::uint32 ||
           type == Type::uint64 || type == Type::ifd || type == Type::ifd64 ||
           type == Type::sint8 || type == Type::sint16 || type == Type::sint32 ||
           type == Type::sint64 || type == Type::undefined;
}

// One integer value of type from its bytes; nullopt when it is a negative signed one.
std::optional<std::uint64_t> integer_of(const Container& container, Type type,
                                        std::span<const std::uint8_t> one) {
    std::uint64_t value = 0;
    std::int64_t signed_value = 0;
    switch (one.size()) {
        case 1:
            value = one[0];
            signed_value = value >= 0x80 ? static_cast<std::int64_t>(value) - 0x100
                                         : static_cast<std::int64_t>(value);
            break;
        case 2:
            value = container.u16(one);
            signed_value = static_cast<std::int16_t>(value);
            break;
        case 4:
            value = container.u32(one);
            signed_value = static_cast<std::int32_t>(value);
            break;
        default:
            value = container.u64(one);
            signed_value = static_cast<std::int64_t>(value);
            break;
    }
    const bool is_signed =
        type == Type::sint8 || type == Type::sint16 || type == Type::sint32 || type == Type::sint64;
    if (is_signed && signed_value < 0) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::vector<std::uint64_t> Ifd::numbers(std::uint16_t tag, std::size_t max_count) const {
    const Entry* entry = find(tag);
    if (entry == nullptr || entry->count == 0 || entry->count > max_count ||
        !is_integer(static_cast<Type>(entry->type))) {
        return {};
    }
    const std::size_t size = type_size(entry->type);
    const std::vector<std::uint8_t> raw = bytes(tag, max_count * size);
    if (raw.size() != entry->count * size) {
        return {};
    }
    std::vector<std::uint64_t> out;
    out.reserve(static_cast<std::size_t>(entry->count));
    for (std::size_t i = 0; i < raw.size(); i += size) {
        const std::optional<std::uint64_t> value =
            integer_of(container_, static_cast<Type>(entry->type), std::span{raw}.subspan(i, size));
        if (!value) {
            return {};
        }
        out.push_back(*value);
    }
    return out;
}

std::vector<double> Ifd::reals(std::uint16_t tag, std::size_t max_count) const {
    const Entry* entry = find(tag);
    if (entry == nullptr || entry->count == 0 || entry->count > max_count) {
        return {};
    }
    const auto type = static_cast<Type>(entry->type);
    if (is_integer(type)) {
        const std::vector<std::uint8_t> raw = bytes(tag, max_count * type_size(entry->type));
        const std::size_t size = type_size(entry->type);
        if (raw.size() != entry->count * size) {
            return {};
        }
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(entry->count));
        const bool is_signed = type == Type::sint8 || type == Type::sint16 ||
                               type == Type::sint32 || type == Type::sint64;
        for (std::size_t i = 0; i < raw.size(); i += size) {
            const std::span<const std::uint8_t> one = std::span{raw}.subspan(i, size);
            std::uint64_t value = 0;
            std::int64_t signed_value = 0;
            switch (size) {
                case 1:
                    value = one[0];
                    signed_value = value >= 0x80 ? static_cast<std::int64_t>(value) - 0x100
                                                 : static_cast<std::int64_t>(value);
                    break;
                case 2:
                    value = container_.u16(one);
                    signed_value = static_cast<std::int16_t>(value);
                    break;
                case 4:
                    value = container_.u32(one);
                    signed_value = static_cast<std::int32_t>(value);
                    break;
                default:
                    value = container_.u64(one);
                    signed_value = static_cast<std::int64_t>(value);
                    break;
            }
            out.push_back(is_signed ? static_cast<double>(signed_value)
                                    : static_cast<double>(value));
        }
        return out;
    }
    if (type != Type::rational && type != Type::srational && type != Type::float32 &&
        type != Type::float64) {
        return {};
    }
    const std::size_t size = type_size(entry->type);
    const std::vector<std::uint8_t> raw = bytes(tag, max_count * size);
    if (raw.size() != entry->count * size) {
        return {};
    }
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(entry->count));
    for (std::size_t i = 0; i < raw.size(); i += size) {
        const std::span<const std::uint8_t> one = std::span{raw}.subspan(i, size);
        switch (type) {
            case Type::rational: {
                const std::uint32_t denominator = container_.u32(one.subspan(4, 4));
                out.push_back(denominator == 0 ? 0.0
                                               : static_cast<double>(container_.u32(one)) /
                                                     static_cast<double>(denominator));
                break;
            }
            case Type::srational: {
                const auto numerator = static_cast<std::int32_t>(container_.u32(one));
                const auto denominator =
                    static_cast<std::int32_t>(container_.u32(one.subspan(4, 4)));
                out.push_back(denominator == 0 ? 0.0
                                               : static_cast<double>(numerator) /
                                                     static_cast<double>(denominator));
                break;
            }
            case Type::float32: {
                const std::uint32_t bits = container_.u32(one);
                float value = 0.0F;
                std::memcpy(&value, &bits, sizeof value);
                out.push_back(static_cast<double>(value));
                break;
            }
            default: {
                const std::uint64_t bits = container_.u64(one);
                double value = 0.0;
                std::memcpy(&value, &bits, sizeof value);
                out.push_back(value);
                break;
            }
        }
    }
    return out;
}

std::optional<std::uint64_t> Ifd::number(std::uint16_t tag) const {
    const Entry* entry = find(tag);
    if (entry == nullptr || entry->count == 0 || !is_integer(static_cast<Type>(entry->type))) {
        return std::nullopt;
    }
    const std::size_t size = type_size(entry->type);
    const std::vector<std::uint8_t> first =
        entry->is_inline ? std::vector<std::uint8_t>{entry->inline_bytes.begin(),
                                                     entry->inline_bytes.begin() +
                                                         static_cast<std::ptrdiff_t>(size)}
                         : container_.source().read(entry->offset, size);
    if (first.size() != size) {
        return std::nullopt;
    }
    return integer_of(container_, static_cast<Type>(entry->type), first);
}

}  // namespace rummage::formats::image::tiff
