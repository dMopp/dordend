#include "rummage/fs/mime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fnmatch.h>
#include <functional>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <utility>

#include "rummage/core/file.hpp"
#include "rummage/core/log.hpp"
#include "rummage/fs/entry.hpp"

namespace rummage::fs {
namespace {

// Bytes read for the text/binary guess when magic needs fewer.
constexpr std::size_t kTextSniffBytes = 4096;
// Upper bound on a head read, whatever the magic rules ask for.
constexpr std::size_t kMaxHeadBytes = std::size_t{64} * 1024;
constexpr int kDefaultGlobWeight = 50;
// A content match this strong overrides conflicting name matches (GLib's threshold).
constexpr int kStrongMagicPriority = 80;

std::string ascii_lower(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return out;
}

bool has_wildcard(std::string_view pattern) {
    return pattern.find_first_of("*?[") != std::string_view::npos;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == separator) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

// xdgmime's binary-or-text fallback: the first 128 bytes hold no control character other than
// tab, line feed and carriage return.
bool text_by_first_bytes(std::span<const std::byte> head) {
    constexpr std::size_t checked_bytes = 128;
    return std::ranges::none_of(head.first(std::min(head.size(), checked_bytes)), [](std::byte b) {
        const auto c = std::to_integer<unsigned>(b);
        return c < 32 && c != '\t' && c != '\n' && c != '\r';
    });
}

// GLib's looks_like_text, the last resort: no ASCII control character but whitespace and
// backspace in the whole head.
bool looks_like_text(std::span<const std::byte> head) {
    return std::ranges::none_of(head, [](std::byte b) {
        const auto c = std::to_integer<unsigned>(b);
        const bool control = c < 32 || c == 127;
        const bool space = c == ' ' || (c >= '\t' && c <= '\r');
        return control && !space && c != '\b';
    });
}

// Lets the glob maps be searched with string_views, without building a std::string per lookup.
struct StringHash {
    using is_transparent = void;  // NOLINT(readability-identifier-naming): standard name

    std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
};

using GlobIndex = std::unordered_multimap<std::string, std::size_t, StringHash, std::equal_to<>>;

struct Glob {
    MimeId mime;
    int weight;
    bool case_sensitive;
    std::size_t dir;  // index of the mime directory it came from, 0 = lowest priority
    std::string pattern;
};

struct MagicRule {
    int indent{0};
    std::uint32_t start{0};
    std::uint32_t range{1};
    std::string value;
    std::string mask;  // empty: no mask
};

struct MagicSection {
    int priority{0};
    MimeId mime{kUnknownMime};
    std::size_t dir{0};
    std::vector<MagicRule>
        rules{};  // NOLINT(readability-redundant-member-init): GCC's -Wmissing-field-initializers
};

bool rule_test(const MagicRule& rule, std::span<const std::byte> data) {
    const std::size_t length = rule.value.size();
    for (std::uint64_t offset = rule.start; offset < std::uint64_t{rule.start} + rule.range;
         ++offset) {
        if (offset + length > data.size()) {
            return false;
        }
        bool equal = true;
        for (std::size_t i = 0; i < length && equal; ++i) {
            auto byte = std::to_integer<unsigned char>(data[offset + i]);
            auto expected = static_cast<unsigned char>(rule.value[i]);
            if (!rule.mask.empty()) {
                const auto mask = static_cast<unsigned char>(rule.mask[i]);
                byte &= mask;
                expected &= mask;
            }
            equal = byte == expected;
        }
        if (equal) {
            return true;
        }
    }
    return false;
}

// A rule matches when its test does and, if it has children (the following rules one level
// deeper), one of them matches too.
// NOLINTNEXTLINE(misc-no-recursion): depth bounded by the rule indents
bool rule_tree_matches(const std::vector<MagicRule>& rules, std::size_t i,
                       std::span<const std::byte> data) {
    if (!rule_test(rules[i], data)) {
        return false;
    }
    bool has_children = false;
    for (std::size_t j = i + 1; j < rules.size() && rules[j].indent > rules[i].indent; ++j) {
        if (rules[j].indent == rules[i].indent + 1) {
            has_children = true;
            if (rule_tree_matches(rules, j, data)) {
                return true;
            }
        }
    }
    return !has_children;
}

bool section_matches(const MagicSection& section, std::span<const std::byte> data) {
    for (std::size_t i = 0; i < section.rules.size(); ++i) {
        if (section.rules[i].indent == 0 && rule_tree_matches(section.rules, i, data)) {
            return true;
        }
    }
    return false;
}

std::vector<std::filesystem::path> default_mime_dirs() {
    std::vector<std::filesystem::path> dirs;
    // NOLINTBEGIN(concurrency-mt-unsafe): read once, before any thread looks at the environment
    const char* data_home = std::getenv("XDG_DATA_HOME");
    const char* home = std::getenv("HOME");
    const char* data_dirs = std::getenv("XDG_DATA_DIRS");
    // NOLINTEND(concurrency-mt-unsafe)
    if (data_home != nullptr && data_home[0] == '/') {
        dirs.emplace_back(std::filesystem::path{data_home} / "mime");
    } else if (home != nullptr && home[0] != '\0') {
        dirs.emplace_back(std::filesystem::path{home} / ".local/share/mime");
    }
    const std::string_view list = (data_dirs != nullptr && data_dirs[0] != '\0')
                                      ? data_dirs
                                      : "/usr/local/share/:/usr/share/";
    for (const std::string_view dir : split(list, ':')) {
        if (!dir.empty() && dir.front() == '/') {
            dirs.emplace_back(std::filesystem::path{dir} / "mime");
        }
    }
    return dirs;
}

}  // namespace

struct MimeDatabase::Impl {
    std::vector<std::string> names{""};
    std::unordered_map<std::string, MimeId> ids;
    std::unordered_map<std::string, std::string> aliases;  // alias -> canonical
    std::unordered_map<MimeId, std::vector<MimeId>> parents;
    std::unordered_map<MimeId, std::string> generic_icons;

    std::vector<Glob> globs;
    GlobIndex literal;              // pattern -> globs index
    GlobIndex suffix;               // text after '*' -> globs index
    std::vector<std::size_t> full;  // fnmatch patterns
    std::size_t longest_suffix{0};
    std::array<bool, 256>
        suffix_starts{};  // first bytes of suffix patterns: other starts can't match

    std::vector<MagicSection> magic;
    MimeId desktop{kUnknownMime};  // application/x-desktop

    MimeId intern(std::string_view name) {
        std::string key{name};
        if (const auto alias = aliases.find(key); alias != aliases.end()) {
            key = alias->second;
        }
        if (const auto it = ids.find(key); it != ids.end()) {
            return it->second;
        }
        const auto id = static_cast<MimeId>(names.size());
        names.push_back(key);
        ids.emplace(std::move(key), id);
        return id;
    }

    void read_aliases(const std::filesystem::path& file) {
        const core::TextFile text = core::read_text_file(file);
        for (const std::string_view line : split(text.text, '\n')) {
            const std::size_t space = line.find(' ');
            if (space != std::string_view::npos && !line.starts_with('#')) {
                aliases.emplace(line.substr(0, space), line.substr(space + 1));
            }
        }
    }

    void read_subclasses(const std::filesystem::path& file) {
        const core::TextFile text = core::read_text_file(file);
        for (const std::string_view line : split(text.text, '\n')) {
            const std::size_t space = line.find(' ');
            if (space != std::string_view::npos && !line.starts_with('#')) {
                std::vector<MimeId>& up = parents[intern(line.substr(0, space))];
                const MimeId parent = intern(line.substr(space + 1));
                if (std::ranges::find(up, parent) == up.end()) {  // several dirs list it
                    up.push_back(parent);
                }
            }
        }
    }

    void read_generic_icons(const std::filesystem::path& file) {
        const core::TextFile text = core::read_text_file(file);
        for (const std::string_view line : split(text.text, '\n')) {
            const std::size_t colon = line.find(':');
            if (colon != std::string_view::npos && !line.starts_with('#')) {
                generic_icons[intern(line.substr(0, colon))] = line.substr(colon + 1);
            }
        }
    }

    void read_globs2(const std::filesystem::path& file, std::size_t dir) {
        const core::TextFile text = core::read_text_file(file);
        for (const std::string_view line : split(text.text, '\n')) {
            if (line.empty() || line.starts_with('#')) {
                continue;
            }
            const std::vector<std::string_view> fields = split(line, ':');
            if (fields.size() < 3) {
                continue;
            }
            int weight = kDefaultGlobWeight;
            std::from_chars(fields[0].data(), fields[0].data() + fields[0].size(), weight);
            const MimeId mime = intern(fields[1]);
            if (fields[2] == "__NOGLOBS__") {
                // Drop this type's globs from lower-priority directories.
                std::erase_if(globs, [&](const Glob& g) { return g.mime == mime && g.dir < dir; });
                continue;
            }
            const bool case_sensitive = fields.size() > 3 && split(fields[3], ',')[0] == "cs";
            globs.push_back(
                {.mime = mime,
                 .weight = weight,
                 .case_sensitive = case_sensitive,
                 .dir = dir,
                 .pattern = case_sensitive ? std::string{fields[2]} : ascii_lower(fields[2])});
        }
    }

    // The binary magic format of the shared-mime-info spec.
    void read_magic(const std::filesystem::path& file, std::size_t dir) {
        const core::TextFile text = core::read_text_file(file);
        if (!text.exists) {
            return;
        }
        constexpr std::string_view header{"MIME-Magic\0\n", 12};
        const std::string_view data = text.text;
        if (!data.starts_with(header)) {
            log::warn("mime: {}: not a magic file", file.string());
            return;
        }
        std::vector<MagicSection> sections;
        std::size_t pos = header.size();
        const auto fail = [&](std::string_view what) {
            log::warn("mime: {}: {} at byte {}; magic from this file ignored", file.string(), what,
                      pos);
            sections.clear();
            pos = data.size();
        };
        const auto read_number = [&](std::uint32_t& out) {
            const auto [end, error] =
                std::from_chars(data.data() + pos, data.data() + data.size(), out);
            if (error != std::errc{} || end == data.data() + pos) {
                return false;
            }
            pos = static_cast<std::size_t>(end - data.data());
            return true;
        };
        while (pos < data.size()) {
            if (data[pos] == '[') {
                const std::size_t end = data.find("]\n", pos);
                const std::size_t colon = data.find(':', pos);
                if (end == std::string_view::npos || colon == std::string_view::npos ||
                    colon > end) {
                    fail("bad section header");
                    break;
                }
                MagicSection section{.dir = dir};
                std::from_chars(data.data() + pos + 1, data.data() + colon, section.priority);
                section.mime = intern(data.substr(colon + 1, end - colon - 1));
                sections.push_back(std::move(section));
                pos = end + 2;
                continue;
            }
            if (sections.empty()) {
                fail("rule before any section");
                break;
            }
            if (data.substr(pos).starts_with("__NOMAGIC__\n")) {
                const MimeId mime = sections.back().mime;
                std::erase_if(magic, [&](const MagicSection& s) { return s.mime == mime; });
                pos += 12;
                continue;
            }
            MagicRule rule;
            std::uint32_t indent = 0;
            if (data[pos] != '>' && !read_number(indent)) {
                fail("bad indent");
                break;
            }
            rule.indent = static_cast<int>(indent);
            if (pos >= data.size() || data[pos] != '>') {
                fail("expected '>'");
                break;
            }
            ++pos;
            if (!read_number(rule.start) || pos >= data.size() || data[pos] != '=') {
                fail("bad offset");
                break;
            }
            ++pos;
            if (pos + 2 > data.size()) {
                fail("truncated value length");
                break;
            }
            const auto length =
                static_cast<std::size_t>((static_cast<unsigned char>(data[pos]) << 8U) |
                                         static_cast<unsigned char>(data[pos + 1]));
            pos += 2;
            if (pos + length > data.size()) {
                fail("truncated value");
                break;
            }
            rule.value = data.substr(pos, length);
            pos += length;
            std::uint32_t word_size = 1;
            while (pos < data.size() && data[pos] != '\n') {
                const char tag = data[pos++];
                if (tag == '&') {
                    if (pos + length > data.size()) {
                        fail("truncated mask");
                        break;
                    }
                    rule.mask = data.substr(pos, length);
                    pos += length;
                } else if (tag == '~') {
                    read_number(word_size);
                } else if (tag == '+') {
                    read_number(rule.range);
                } else {
                    // Unknown extension: the spec says to skip the rest of the line.
                    pos = data.find('\n', pos);
                    if (pos == std::string_view::npos) {
                        pos = data.size();
                    }
                    break;
                }
            }
            ++pos;  // '\n'
            if constexpr (std::endian::native == std::endian::little) {
                // Values and masks of multi-byte words are big-endian in the file.
                if (word_size == 2 || word_size == 4) {
                    for (std::string* bytes : {&rule.value, &rule.mask}) {
                        for (std::size_t i = 0; i + word_size <= bytes->size(); i += word_size) {
                            std::reverse(
                                bytes->begin() + static_cast<std::ptrdiff_t>(i),
                                bytes->begin() + static_cast<std::ptrdiff_t>(i + word_size));
                        }
                    }
                }
            }
            rule.range = std::max<std::uint32_t>(rule.range, 1);
            sections.back().rules.push_back(std::move(rule));
        }
        std::ranges::move(sections, std::back_inserter(magic));
    }

    void build_indexes() {
        for (std::size_t i = 0; i < globs.size(); ++i) {
            const std::string& pattern = globs[i].pattern;
            if (!has_wildcard(pattern)) {
                literal.emplace(pattern, i);
            } else if (pattern.starts_with('*') && !has_wildcard(pattern.substr(1))) {
                suffix.emplace(pattern.substr(1), i);
                if (pattern.size() > 1) {
                    suffix_starts.at(static_cast<unsigned char>(pattern[1])) = true;
                }
                longest_suffix = std::max(longest_suffix, pattern.size() - 1);
            } else {
                full.push_back(i);
            }
        }
        // Highest priority first; stable keeps file order within a priority.
        std::ranges::stable_sort(magic, std::greater{}, &MagicSection::priority);
    }

    struct Sniffed {
        MimeId mime{kUnknownMime};
        int priority{0};
    };

    // Highest-priority magic section that matches.
    [[nodiscard]] Sniffed sniff(std::span<const std::byte> head) const {
        for (const MagicSection& section : magic) {
            if (section_matches(section, head)) {
                return {section.mime, section.priority};
            }
        }
        return {};
    }

    struct Candidate {
        MimeId mime;
        int weight;
        std::size_t length;
        std::size_t order;  // position in the globs files: the tie-break, as in GLib
    };

    // Glob matches as GLib's xdgmime collects them: a literal name wins outright; otherwise the
    // longest matching suffix; full patterns (fnmatch) only when neither matched. At each step a
    // case-sensitive match beats the case-insensitive ones. Distinct types, highest weight first,
    // then longest pattern, then file order. More than one type: the content has to decide.
    [[nodiscard]] std::vector<MimeId> name_candidates(std::string_view name) const {
        const std::string lower = ascii_lower(name);
        std::vector<Candidate> matches;
        // Case-sensitive globs match the name as is, the others its lower-case form.
        const auto collect = [&](const GlobIndex& map, std::string_view original,
                                 std::string_view folded) {
            for (const bool case_sensitive : {true, false}) {
                const auto [first, last] = map.equal_range(case_sensitive ? original : folded);
                for (auto it = first; it != last; ++it) {
                    const Glob& glob = globs[it->second];
                    if (glob.case_sensitive == case_sensitive) {
                        matches.push_back(
                            {glob.mime, glob.weight, glob.pattern.size(), it->second});
                    }
                }
                if (!matches.empty()) {
                    return;
                }
            }
        };
        collect(literal, name, lower);
        const std::size_t from = name.size() > longest_suffix ? name.size() - longest_suffix : 0;
        for (std::size_t start = from; start < name.size() && matches.empty(); ++start) {
            if (suffix_starts.at(static_cast<unsigned char>(name[start])) ||
                suffix_starts.at(static_cast<unsigned char>(lower[start]))) {
                collect(suffix, name.substr(start), std::string_view{lower}.substr(start));
            }
        }
        if (matches.empty() && !full.empty()) {
            const std::string original{name};  // fnmatch needs NUL-terminated strings
            for (const std::size_t index : full) {
                const Glob& glob = globs[index];
                const std::string& tested = glob.case_sensitive ? original : lower;
                if (fnmatch(glob.pattern.c_str(), tested.c_str(), FNM_NOESCAPE) == 0) {
                    matches.push_back({glob.mime, glob.weight, glob.pattern.size(), index});
                }
            }
        }
        std::ranges::sort(matches, [](const Candidate& a, const Candidate& b) {
            if (a.weight != b.weight) {
                return a.weight > b.weight;
            }
            if (a.length != b.length) {
                return a.length > b.length;
            }
            return a.order < b.order;
        });
        std::vector<MimeId> result;
        for (const Candidate& c : matches) {
            if (std::ranges::find(result, c.mime) == result.end()) {
                result.push_back(c.mime);
            }
        }
        return result;
    }
};

MimeDatabase::MimeDatabase() : impl_{std::make_unique<Impl>()} {
    for (const std::string_view name :
         {"application/octet-stream", "text/plain", "inode/directory", "application/x-zerosize",
          "inode/symlink", "inode/x-special"}) {
        impl_->intern(name);
    }
    impl_->desktop = impl_->intern("application/x-desktop");
}

MimeDatabase::~MimeDatabase() = default;

std::shared_ptr<const MimeDatabase> MimeDatabase::load() { return load(default_mime_dirs()); }

std::shared_ptr<const MimeDatabase> MimeDatabase::load(
    const std::vector<std::filesystem::path>& mime_dirs) {
    std::shared_ptr<MimeDatabase> db{new MimeDatabase};  // private constructor
    Impl& impl = *db->impl_;
    // Lowest priority first, so higher directories can override (__NOGLOBS__, __NOMAGIC__).
    for (std::size_t i = mime_dirs.size(); i-- > 0;) {
        impl.read_aliases(mime_dirs[i] / "aliases");
    }
    for (std::size_t i = mime_dirs.size(); i-- > 0;) {
        const std::size_t dir = mime_dirs.size() - 1 - i;
        impl.read_subclasses(mime_dirs[i] / "subclasses");
        impl.read_generic_icons(mime_dirs[i] / "generic-icons");
        impl.read_globs2(mime_dirs[i] / "globs2", dir);
        impl.read_magic(mime_dirs[i] / "magic", dir);
    }
    impl.build_indexes();
    std::size_t extent = 0;
    for (const MagicSection& section : impl.magic) {
        for (const MagicRule& rule : section.rules) {
            extent = std::max<std::size_t>(
                extent, std::size_t{rule.start} + rule.range - 1 + rule.value.size());
        }
    }
    db->magic_extent_ = extent;
    log::debug("mime: {} types, {} globs, {} magic sections, head {} bytes", impl.names.size() - 1,
               impl.globs.size(), impl.magic.size(), extent);
    return db;
}

MimeDatabase::NameMatch MimeDatabase::by_name(std::string_view file_name) const {
    const std::vector<MimeId> candidates = impl_->name_candidates(file_name);
    if (candidates.empty()) {
        return {};
    }
    return {.mime = candidates.front(), .certain = candidates.size() == 1};
}

MimeId MimeDatabase::by_content(std::span<const std::byte> head) const {
    return impl_->sniff(head).mime;
}

// The order of GLib's g_content_type_guess, so rummage agrees with GTK apps and gio.
MimeId MimeDatabase::to_open(MimeId mime, std::string_view file_name) const {
    if (mime != kZeroSize) {
        return mime;
    }
    const MimeId named = by_name(file_name).mime;
    return named == kUnknownMime ? mime : named;
}

MimeId MimeDatabase::detect(std::string_view file_name, std::span<const std::byte> head,
                            std::uint64_t size) const {
    if (size == 0) {
        return kZeroSize;  // as GLib: an empty file is empty, whatever its name
    }
    const std::vector<MimeId> candidates = impl_->name_candidates(file_name);
    if (candidates.size() == 1) {
        return candidates.front();
    }
    Impl::Sniffed sniffed = impl_->sniff(head);
    if (sniffed.mime == kUnknownMime && (text_by_first_bytes(head) || looks_like_text(head))) {
        sniffed.mime = kTextPlain;
    }
    // A desktop file can launch any program: never trust one that is not named *.desktop.
    if (sniffed.mime == impl_->desktop && !ascii_lower(file_name).ends_with(".desktop")) {
        sniffed.mime = kTextPlain;
    }
    if (candidates.empty()) {
        return sniffed.mime != kUnknownMime ? sniffed.mime : kOctetStream;
    }
    if (sniffed.mime != kUnknownMime) {
        if (sniffed.priority >= kStrongMagicPriority) {
            return sniffed.mime;
        }
        // Names disagree: the content breaks the tie, preferring the more precise name type.
        for (const MimeId candidate : candidates) {
            if (is_a(candidate, sniffed.mime)) {
                return candidate;
            }
        }
    }
    return candidates.front();
}

MimeId MimeDatabase::detect(const std::filesystem::path& directory, const Entry& entry) const {
    switch (entry.kind) {
        case EntryKind::directory:
            return kDirectory;
        case EntryKind::other:
            return entry.is_link ? kSymlink : kSpecial;
        case EntryKind::file:
        case EntryKind::unknown:
            break;
    }
    if (entry.size == 0) {
        return kZeroSize;
    }
    const NameMatch by_file_name = by_name(entry.name);
    if (by_file_name.certain) {
        return by_file_name.mime;
    }
    const std::size_t wanted = std::min(std::max(magic_extent_, kTextSniffBytes), kMaxHeadBytes);
    std::vector<std::byte> head(std::min<std::uint64_t>(entry.size, wanted));
    if (!head.empty()) {
        const int fd = open((directory / entry.name).c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return by_file_name.mime != kUnknownMime ? by_file_name.mime : kOctetStream;
        }
        const ssize_t got = pread(fd, head.data(), head.size(), 0);
        close(fd);
        head.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
    }
    return detect(entry.name, head, entry.size);
}

std::string_view MimeDatabase::name(MimeId mime) const {
    return mime < impl_->names.size() ? std::string_view{impl_->names[mime]} : std::string_view{};
}

MimeId MimeDatabase::find(std::string_view name) const {
    std::string key{name};
    if (const auto alias = impl_->aliases.find(key); alias != impl_->aliases.end()) {
        key = alias->second;
    }
    const auto it = impl_->ids.find(key);
    return it == impl_->ids.end() ? kUnknownMime : it->second;
}

bool MimeDatabase::is_a(MimeId mime, MimeId parent) const { return is_a(mime, parent, 0); }

// NOLINTNEXTLINE(misc-no-recursion): depth capped by max_depth
bool MimeDatabase::is_a(MimeId mime, MimeId parent, int depth) const {
    constexpr int max_depth = 16;  // a broken subclasses file must not loop forever
    if (mime == parent) {
        return true;
    }
    if (mime == kUnknownMime || parent == kUnknownMime) {
        return false;
    }
    const std::string_view type = name(mime);
    if (parent == kOctetStream) {
        return !type.starts_with("inode/");
    }
    if (parent == kTextPlain && type.starts_with("text/")) {
        return true;
    }
    if (const auto it = impl_->parents.find(mime);
        it != impl_->parents.end() && depth < max_depth) {
        for (const MimeId up : it->second) {
            if (is_a(up, parent, depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

std::span<const MimeId> MimeDatabase::parents(MimeId mime) const {
    const auto it = impl_->parents.find(mime);
    return it == impl_->parents.end() ? std::span<const MimeId>{} : std::span{it->second};
}

std::string MimeDatabase::generic_icon(MimeId mime) const {
    if (const auto it = impl_->generic_icons.find(mime); it != impl_->generic_icons.end()) {
        return it->second;
    }
    const std::string_view type = name(mime);
    const std::size_t slash = type.find('/');
    return slash == std::string_view::npos ? std::string{}
                                           : std::string{type.substr(0, slash)} + "-x-generic";
}

std::size_t MimeDatabase::glob_count() const noexcept { return impl_->globs.size(); }

std::size_t MimeDatabase::magic_count() const noexcept { return impl_->magic.size(); }

}  // namespace rummage::fs
