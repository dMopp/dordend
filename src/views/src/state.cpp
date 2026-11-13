#include "rummage/views/state.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <optional>
#include <system_error>
#include <toml++/toml.hpp>
#include <utility>

#include "rummage/core/file.hpp"
#include "rummage/core/log.hpp"

namespace rummage::views {
namespace {

constexpr std::array kModes{std::pair{"columns", State::Mode::columns},
                            std::pair{"grid", State::Mode::grid},
                            std::pair{"details", State::Mode::details}};
constexpr std::array kListSizeWords{
    std::pair{"small", std::size_t{0}}, std::pair{"usual", std::size_t{1}},
    std::pair{"large", std::size_t{2}}, std::pair{"larger", std::size_t{3}},
    std::pair{"largest", std::size_t{4}}};
constexpr std::array kSortKeys{
    std::pair{"name", fs::SortKey::name}, std::pair{"size", fs::SortKey::size},
    std::pair{"modified", fs::SortKey::modified}, std::pair{"type", fs::SortKey::type}};

// The limits the views themselves keep (Sidebar, InfoPane, DetailsView, WindowOptions).
constexpr float kSidebarMin = 140.0F;
constexpr float kSidebarMax = 400.0F;
constexpr float kInfoMin = 240.0F;
constexpr float kInfoMax = 720.0F;
constexpr float kColumnMin = 48.0F;
constexpr float kColumnMax = 800.0F;
constexpr std::int64_t kWindowMin = 320;
constexpr std::int64_t kWindowMax = 16384;

std::string grid_size_choices() {
    std::string choices;
    for (const auto size : kGridIconSizes) {
        if (!choices.empty()) {
            choices += ", ";
        }
        choices += std::to_string(size);
    }
    return choices;
}

class Reader {
public:
    Reader(const toml::table& root, std::string_view source) : root_{root}, source_{source} {}

    void flag(std::string_view key, bool& out) {
        if (const toml::node* node = find(key)) {
            if (const auto value = node->value<bool>()) {
                out = *value;
            } else {
                bad(*node, key, "true or false");
            }
        }
    }

    void number(std::string_view key, float min, float max, float& out) {
        if (const toml::node* node = find(key)) {
            const auto value = node->value<double>();
            if (value && *value >= static_cast<double>(min) && *value <= static_cast<double>(max)) {
                out = static_cast<float>(*value);
            } else {
                bad(*node, key, std::format("a number from {} to {}", min, max));
            }
        }
    }

    void size(std::string_view key, std::int32_t& out) {
        if (const toml::node* node = find(key)) {
            const auto value = node->value<std::int64_t>();
            if (value && *value >= kWindowMin && *value <= kWindowMax) {
                out = static_cast<std::int32_t>(*value);
            } else {
                bad(*node, key,
                    std::format("a whole number from {} to {}", kWindowMin, kWindowMax));
            }
        }
    }

    template <typename T, std::size_t N>
    void word(std::string_view key, const std::array<std::pair<const char*, T>, N>& words, T& out) {
        if (const toml::node* node = find(key)) {
            const auto value = node->value<std::string_view>();
            for (const auto& [text, meaning] : words) {
                if (value == text) {
                    out = meaning;
                    return;
                }
            }
            std::string choices;
            for (const auto& [text, meaning] : words) {
                choices += std::format("{}\"{}\"", choices.empty() ? "" : ", ", text);
            }
            bad(*node, key, "one of " + choices);
        }
    }

    [[nodiscard]] std::vector<std::string> take() { return std::move(problems_); }

private:
    // "a.b" looks into the table a.
    [[nodiscard]] const toml::node* find(std::string_view key) const {
        return root_.at_path(key).node();
    }

    void bad(const toml::node& node, std::string_view key, std::string_view what) {
        const toml::source_position where = node.source().begin;
        problems_.push_back(std::format("{}:{}:{}: '{}' must be {}; using the default", source_,
                                        where.line, where.column, key, what));
    }

    const toml::table& root_;
    std::string_view source_;
    std::vector<std::string> problems_;
};

// A TOML basic string: quotes, backslashes and control characters escaped. text must be UTF-8.
std::string toml_string(std::string_view text) {
    std::string out{"\""};
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20U || byte == 0x7FU) {
            out += std::format("\\u{:04X}", static_cast<unsigned>(byte));
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

// Well-formed UTF-8 (TOML text is): no stray, overlong or surrogate sequences.
bool valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        std::uint32_t point = lead;
        if (lead >= 0xF0U && lead <= 0xF4U) {
            length = 4;
            point = lead & 0x07U;
        } else if (lead >= 0xE0U) {
            length = 3;
            point = lead & 0x0FU;
        } else if (lead >= 0xC2U && lead <= 0xDFU) {
            length = 2;
            point = lead & 0x1FU;
        } else if (lead >= 0x80U) {
            return false;
        }
        if (i + length > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            point = (point << 6U) | (next & 0x3FU);
        }
        constexpr std::array<std::uint32_t, 5> least{0, 0, 0x80, 0x800, 0x10000};
        if (point < least.at(length) || point > 0x10FFFFU ||
            (point >= 0xD800U && point <= 0xDFFFU)) {
            return false;
        }
        i += length;
    }
    return true;
}

template <typename T, std::size_t N>
const char* word_of(const std::array<std::pair<const char*, T>, N>& words, T value) {
    for (const auto& [text, meaning] : words) {
        if (meaning == value) {
            return text;
        }
    }
    return words.front().first;
}

}  // namespace

std::filesystem::path default_state_path() {
    const char* xdg = std::getenv("XDG_STATE_HOME");  // NOLINT(concurrency-mt-unsafe)
    // The XDG base directory spec ignores relative paths.
    if (xdg != nullptr && xdg[0] == '/') {
        return std::filesystem::path{xdg} / "rummage" / "state.toml";
    }
    const char* home = std::getenv("HOME");  // NOLINT(concurrency-mt-unsafe)
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".local" / "state" / "rummage" / "state.toml";
    }
    return {};
}

StateResult parse_state(std::string_view text, std::string_view source) {
    StateResult result;
    toml::table root;
    try {
        root = toml::parse(text, source);
    } catch (const toml::parse_error& e) {
        const toml::source_position where = e.source().begin;
        result.problems.push_back(std::format("{}:{}:{}: {}; using the defaults", source,
                                              where.line, where.column, e.description()));
        return result;
    }
    State& s = result.state;
    Reader read{root, source};
    read.word("view.mode", kModes, s.mode);
    read.flag("view.show_hidden", s.show_hidden);
    read.word("view.list_size", kListSizeWords, s.list_size);
    read.flag("sidebar.visible", s.sidebar_visible);
    read.word("sidebar.list_size", kListSizeWords, s.sidebar_size);
    read.number("sidebar.width", kSidebarMin, kSidebarMax, s.sidebar_width);
    read.flag("info.visible", s.info_visible);
    read.number("info.width", kInfoMin, kInfoMax, s.info_width);
    read.flag("info.expanded", s.info_expanded);
    read.word("details.sort", kSortKeys, s.sort_key);
    read.flag("details.descending", s.sort_descending);
    read.number("details.size_width", kColumnMin, kColumnMax, s.details_widths[0]);
    read.number("details.modified_width", kColumnMin, kColumnMax, s.details_widths[1]);
    read.number("details.type_width", kColumnMin, kColumnMax, s.details_widths[2]);
    read.size("window.width", s.window_width);
    read.size("window.height", s.window_height);
    read.flag("window.maximized", s.maximized);
    read.flag("panes.visible", s.split_visible);
    read.flag("panes.initialized", s.panes_initialized);
    read.flag("panes.stacked", s.stacked);
    read.number("panes.ratio", 0.15F, 0.85F, s.split_ratio);
    s.active_pane = root.at_path("panes.active").value_or<std::int64_t>(0) == 1 ? 1 : 0;
    result.problems = read.take();
    if (const toml::node* grid = root.at_path("view.grid_icon").node()) {
        const std::optional<std::int64_t> pixels = grid->value<std::int64_t>();
        if (pixels && std::ranges::find(kGridIconSizes, *pixels) != kGridIconSizes.end()) {
            s.grid_icon = static_cast<std::int32_t>(*pixels);
        } else {
            const toml::source_position where = grid->source().begin;
            result.problems.push_back(
                std::format("{}:{}:{}: 'view.grid_icon' must be one of {}; using the default",
                            source, where.line, where.column, grid_size_choices()));
        }
    }

    // [palette] recent: a list of ids; what is not a string is left out.
    if (const toml::node* recent = root.at_path("palette.recent").node()) {
        if (const toml::array* list = recent->as_array()) {
            for (const toml::node& id : *list) {
                const std::optional<std::string_view> value = id.value<std::string_view>();
                if (value && !value->empty() && s.palette_recent.size() < State::kMaxRecent) {
                    s.palette_recent.emplace_back(*value);
                }
            }
        } else {
            const toml::source_position where = recent->source().begin;
            result.problems.push_back(
                std::format("{}:{}:{}: 'palette.recent' must be a list of strings; left out",
                            source, where.line, where.column));
        }
    }

    // [[tab]]: folder, mode, root (the columns'), shown. A tab that cannot be used is left out.
    if (const toml::node* tabs = root.get("tab")) {
        const auto problem = [&](const toml::node& node, std::string_view what) {
            const toml::source_position where = node.source().begin;
            result.problems.push_back(std::format("{}:{}:{}: {}; that tab is left out", source,
                                                  where.line, where.column, what));
        };
        const toml::array* list = tabs->as_array();
        if (list == nullptr) {
            problem(*tabs, "'tab' must be a list of tables ([[tab]])");
        } else {
            for (const toml::node& node : *list) {
                const toml::table* table = node.as_table();
                if (table == nullptr) {
                    problem(node, "a tab must be a table");
                    continue;
                }
                if (static_cast<std::size_t>(std::ranges::count(
                        s.tabs,
                        (*table)["pane"].value_or<std::int64_t>(0) == 1 ? std::size_t{1}
                                                                        : std::size_t{0},
                        &State::Tab::pane)) == State::kMaxTabs) {
                    problem(node, std::format("more than {} tabs in a pane", State::kMaxTabs));
                    continue;
                }
                State::Tab tab;
                const std::optional<std::string_view> folder =
                    (*table)["folder"].value<std::string_view>();
                if (!folder || folder->empty() || folder->front() != '/') {
                    problem(node, "'folder' must be an absolute path");
                    continue;
                }
                tab.folder = std::filesystem::path{*folder};
                if (const toml::node* mode = table->get("mode")) {
                    const std::optional<std::string_view> word = mode->value<std::string_view>();
                    const auto* found = std::ranges::find_if(
                        kModes, [&](const auto& pair) { return word == pair.first; });
                    if (found == kModes.end()) {
                        problem(*mode, R"('mode' must be one of "columns", "grid", "details")");
                        continue;
                    }
                    tab.mode = found->second;
                }
                if (const toml::node* from = table->get("root")) {
                    const std::optional<std::string_view> given = from->value<std::string_view>();
                    if (!given || given->empty() || given->front() != '/') {
                        problem(*from, "'root' must be an absolute path");
                        continue;
                    }
                    tab.root = std::filesystem::path{*given};
                }
                if (const toml::node* name = table->get("cursor")) {
                    const std::optional<std::string_view> given = name->value<std::string_view>();
                    if (!given || given->find('/') != std::string_view::npos) {
                        problem(*name, "'cursor' must be a name in the folder");
                        continue;
                    }
                    tab.cursor = std::string{*given};
                }
                if (const toml::node* size = table->get("list_size")) {
                    const std::optional<std::string_view> word = size->value<std::string_view>();
                    const auto* found = std::ranges::find_if(
                        kListSizeWords, [&](const auto& pair) { return word == pair.first; });
                    if (found == kListSizeWords.end()) {
                        problem(
                            *size,
                            R"('list_size' must be one of "small", "usual", "large", "larger", "largest")");
                        continue;
                    }
                    tab.list_size = found->second;
                }
                if (const toml::node* grid = table->get("grid_icon")) {
                    const std::optional<std::int64_t> pixels = grid->value<std::int64_t>();
                    if (!pixels ||
                        std::ranges::find(kGridIconSizes, *pixels) == kGridIconSizes.end()) {
                        problem(*grid,
                                std::format("'grid_icon' must be one of {}", grid_size_choices()));
                        continue;
                    }
                    tab.grid_icon = static_cast<std::int32_t>(*pixels);
                }
                tab.pane = (*table)["pane"].value_or<std::int64_t>(0) == 1 ? 1 : 0;
                if (const auto key = (*table)["sort"].value<std::string>()) {
                    const auto found = std::ranges::find(
                        kSortKeys, *key, [](const auto& item) { return item.first; });
                    if (found != kSortKeys.end())
                        tab.sort_key = found->second;
                }
                tab.sort_descending = (*table)["descending"].value_or(false);
                tab.show_hidden = (*table)["show_hidden"].value<bool>();
                tab.filter = (*table)["filter"].value_or(std::string{});
                if ((*table)["shown"].value_or(false)) {
                    const auto index = static_cast<std::size_t>(
                        std::ranges::count(s.tabs, tab.pane, &State::Tab::pane));
                    if (tab.pane == 0)
                        s.shown_tab = index;
                    else
                        s.other_shown_tab = index;
                }
                s.tabs.push_back(std::move(tab));
            }
        }
    }
    return result;
}

StateResult load_state(const std::filesystem::path& path) {
    const core::TextFile file = core::read_text_file(path);
    if (!file.exists) {
        return {};
    }
    if (!file.error.empty()) {
        StateResult result;
        result.problems.push_back(std::format("{}: {}", path.string(), file.error));
        return result;
    }
    return parse_state(file.text, path.string());
}

std::string format_state(const State& s) {
    const auto flag = [](bool value) { return value ? "true" : "false"; };
    std::string recent;
    for (std::size_t i = 0; i < s.palette_recent.size() && i < State::kMaxRecent; ++i) {
        if (valid_utf8(s.palette_recent[i])) {
            recent +=
                std::format("{}{}", recent.empty() ? "" : ", ", toml_string(s.palette_recent[i]));
        }
    }
    std::string tabs;
    for (std::size_t i = 0; i < s.tabs.size(); ++i) {
        const State::Tab& tab = s.tabs[i];
        const std::string folder = tab.folder.string();
        const std::string root = tab.root.string();
        if (!valid_utf8(folder) || !valid_utf8(root) || !valid_utf8(tab.cursor)) {
            continue;  // TOML holds only UTF-8: a tab on such a name is not remembered
        }
        tabs += std::format("\n[[tab]]\nfolder = {}\nmode = \"{}\"\n", toml_string(folder),
                            word_of(kModes, tab.mode));
        if (!root.empty()) {
            tabs += std::format("root = {}\n", toml_string(root));
        }
        if (!tab.cursor.empty()) {
            tabs += std::format("cursor = {}\n", toml_string(tab.cursor));
        }
        if (tab.list_size && *tab.list_size < kListSizeWords.size()) {
            tabs += std::format("list_size = \"{}\"\n", word_of(kListSizeWords, *tab.list_size));
        }
        if (tab.grid_icon) {
            tabs += std::format("grid_icon = {}\n", *tab.grid_icon);
        }
        tabs += std::format("pane = {}\n", tab.pane);
        if (tab.sort_key)
            tabs += std::format("sort = \"{}\"\ndescending = {}\n",
                                word_of(kSortKeys, *tab.sort_key), flag(tab.sort_descending));
        if (tab.show_hidden)
            tabs += std::format("show_hidden = {}\n", flag(*tab.show_hidden));
        if (!tab.filter.empty() && valid_utf8(tab.filter))
            tabs += "filter = " + toml_string(tab.filter) + "\n";
        const auto index = static_cast<std::size_t>(
            std::count_if(s.tabs.begin(), s.tabs.begin() + static_cast<std::ptrdiff_t>(i),
                          [&tab](const State::Tab& other) { return other.pane == tab.pane; }));
        if (index == (tab.pane == 0 ? s.shown_tab : s.other_shown_tab))
            tabs += "shown = true\n";
    }
    return std::format(
               "# How rummage was left, read at start. Written by rummage: no need to edit it.\n"
               "\n"
               "[view]\n"
               "mode = \"{}\"\n"
               "show_hidden = {}\n"
               "list_size = \"{}\"\n"
               "grid_icon = {}\n"
               "\n"
               "[sidebar]\n"
               "list_size = \"{}\"\n"
               "visible = {}\n"
               "width = {}\n"
               "\n"
               "[info]\n"
               "visible = {}\n"
               "width = {}\n"
               "expanded = {}\n"
               "\n"
               "[details]\n"
               "sort = \"{}\"\n"
               "descending = {}\n"
               "size_width = {}\n"
               "modified_width = {}\n"
               "type_width = {}\n"
               "\n"
               "[window]\n"
               "width = {}\n"
               "height = {}\n"
               "maximized = {}\n"
               "\n"
               "[palette]\n"
               "recent = [{}]\n",
               word_of(kModes, s.mode), flag(s.show_hidden), word_of(kListSizeWords, s.list_size),
               s.grid_icon, word_of(kListSizeWords, s.sidebar_size), flag(s.sidebar_visible),
               s.sidebar_width, flag(s.info_visible), s.info_width, flag(s.info_expanded),
               word_of(kSortKeys, s.sort_key), flag(s.sort_descending), s.details_widths[0],
               s.details_widths[1], s.details_widths[2], s.window_width, s.window_height,
               flag(s.maximized), recent) +
           std::format(
               "\n[panes]\nvisible = {}\ninitialized = {}\nstacked = {}\nratio = {}\nactive = {}\n",
               flag(s.split_visible), flag(s.panes_initialized), flag(s.stacked), s.split_ratio,
               s.active_pane) +
           tabs;
}

// Writes in generation order: one older than the last written is dropped.
struct StateKeeper::File {
    std::filesystem::path path;
    std::mutex mutex;
    std::uint64_t written{0};

    std::string write(std::uint64_t generation, const std::string& text) {
        const std::scoped_lock lock{mutex};
        if (generation <= written) {
            return {};
        }
        written = generation;
        const std::error_code error = core::write_file_atomically(path, text);
        return error ? error.message() : std::string{};
    }
};

StateKeeper::StateKeeper(core::EventLoop& loop, core::WorkerPool& pool, std::filesystem::path path,
                         State saved, std::chrono::milliseconds delay)
    : loop_{loop},
      pool_{pool},
      path_{std::move(path)},
      delay_{delay},
      saved_{saved},
      current_{std::move(saved)},
      file_{std::make_shared<File>()} {
    file_->path = path_;
}

StateKeeper::~StateKeeper() { loop_.remove(timer_); }

void StateKeeper::note(const State& state) {
    if (state == current_) {
        return;
    }
    current_ = state;
    ++generation_;
    if (path_.empty() || writing_) {
        return;  // a write in flight checks again when it ends
    }
    loop_.remove(timer_);
    timer_ = loop_.add_timer(delay_, std::chrono::milliseconds{0}, [this] {
        timer_ = 0;
        write();
    });
}

void StateKeeper::write() {
    if (current_ == saved_ || path_.empty()) {
        return;
    }
    saved_ = current_;
    writing_ = true;
    pool_.submit(
        core::Priority::prefetch,
        [file = file_, generation = generation_, text = format_state(current_)](
            const core::CancelToken& /*token*/) { return file->write(generation, text); },
        [this](const std::string& error) {
            writing_ = false;
            if (!error.empty()) {
                log::warn("state: {}: {}", path_.string(), error);
            }
            write();  // what changed meanwhile
        });
}

void StateKeeper::flush() {
    loop_.remove(timer_);
    timer_ = 0;
    if (current_ == saved_ || path_.empty()) {
        return;
    }
    saved_ = current_;
    if (const std::string error = file_->write(generation_, format_state(saved_)); !error.empty()) {
        log::warn("state: {}: {}", path_.string(), error);
    }
}

}  // namespace rummage::views
