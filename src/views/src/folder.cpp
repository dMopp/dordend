#include "rummage/views/folder.hpp"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

namespace rummage::views {
namespace {

// Sniffing batches stay small so a fast scroll does not queue minutes of reads.
constexpr std::size_t kMaxSniffBatch = 64;

bool hidden_name(const std::string& name) { return !name.empty() && name.front() == '.'; }

}  // namespace

Folder::Folder(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
               std::shared_ptr<const fs::MimeDatabase> mime, std::filesystem::path directory,
               std::function<void()> redraw, bool show_hidden)
    : pool_{pool},
      mime_{std::move(mime)},
      redraw_{std::move(redraw)},
      model_{loop, pool, watcher, mime_} {
    model_.set_change_handler([this](const fs::ModelChange& change) { on_change(change); });
    model_.set_filter(show_hidden, {});
    model_.open(std::move(directory));
}

Folder::~Folder() { *alive_ = false; }

const fs::Entry* Folder::cursor_entry() const {
    return cursor_index_ ? &entries()[*cursor_index_] : nullptr;
}

const fs::Entry* Folder::single_selection() const {
    const fs::Entry* const entry = cursor_entry();
    if (entry == nullptr || selected_.size() != 1 || !selected_.contains(entry->name)) {
        return nullptr;
    }
    return entry;
}

std::vector<std::string> Folder::selected_names() const {
    std::vector<std::string> names;
    names.reserve(selected_.size());
    for (const fs::Entry& entry : entries()) {
        if (selected_.contains(entry.name)) {
            names.push_back(entry.name);
        }
    }
    return names;
}

const SelectionSummary& Folder::summary() const {
    if (!summary_) {
        SelectionSummary summary;
        for (const fs::Entry& entry : entries()) {
            if (!selected_.contains(entry.name)) {
                continue;
            }
            if (entry.is_directory()) {
                ++summary.folders;
            } else {
                ++summary.files;
                summary.bytes += entry.size;
            }
        }
        summary_ = summary;
    }
    return *summary_;
}

Folder::Reveal Folder::take_reveal() noexcept {
    const Reveal reveal = cursor_index_ ? reveal_ : Reveal::none;
    reveal_ = Reveal::none;
    return reveal;
}

void Folder::selection_changed() {
    summary_.reset();
    redraw_();
    if (changed_) {
        changed_();
    }
}

void Folder::set_order(const fs::SortOrder& order) {
    if (order == this->order()) {
        return;  // already so: no re-sort, no jump
    }
    model_.set_order(order);
    reveal_ = Reveal::jump;
    redraw_();
}

void Folder::on_change(const fs::ModelChange& change) {
    for (const std::string& name : change.removed) {
        sniffed_.erase(name);
        selected_.erase(name);
    }
    for (const std::string& name : change.updated) {
        sniffed_.erase(name);  // the content may be different now
    }
    for (const auto& [from, to] : change.renamed) {
        sniffed_.erase(from);
        if (selected_.erase(from) > 0) {
            selected_.insert(to);
        }
        if (from == cursor_name_) {
            cursor_name_ = to;
        }
        if (from == anchor_) {
            anchor_ = to;
        }
    }

    const bool had_cursor = !cursor_name_.empty();
    const bool had_selection = !selected_.empty();
    // A reset (reload after lost events) is followed by a new load: wait for it.
    const bool final_listing = model_.error() || (!change.reset && !model_.loading());
    const std::vector<fs::Entry>& list = entries();
    const std::optional<std::size_t> before = cursor_index_;
    cursor_index_.reset();
    if (had_cursor) {
        const auto it = std::ranges::find(list, cursor_name_, &fs::Entry::name);
        if (it != list.end()) {
            cursor_index_ = static_cast<std::size_t>(it - list.begin());
            if (!before) {
                reveal_ = Reveal::jump;  // it just landed
            }
        } else if (final_listing && !cursor_expected_) {
            cursor_name_.clear();  // gone, or it never came
        }
        if (cursor_index_) {
            cursor_expected_ = false;
        }
    }
    if (had_selection && final_listing && (change.loaded || model_.error())) {
        // After a (re)load, names that did not come back are gone.
        std::unordered_set<std::string> present;
        for (const fs::Entry& entry : list) {
            if (selected_.contains(entry.name)) {
                present.insert(entry.name);
            }
        }
        selected_ = std::move(present);
    }
    if (had_cursor || had_selection) {
        selection_changed();
    } else {
        redraw_();
    }
}

void Folder::set_cursor(std::optional<std::size_t> index, SelectMode mode) {
    cursor_expected_ = false;
    const std::vector<fs::Entry>& list = entries();
    if (!index || list.empty()) {
        reveal_ = Reveal::none;
        if (cursor_name_.empty() && selected_.empty()) {
            return;
        }
        cursor_name_.clear();
        cursor_index_.reset();
        anchor_.clear();
        selected_.clear();
        selection_changed();
        return;
    }
    const std::size_t target = std::min(*index, list.size() - 1);
    const std::string& name = list[target].name;
    switch (mode) {
        case SelectMode::replace:
            selected_ = {name};
            anchor_ = name;
            break;
        case SelectMode::extend: {
            const auto anchor = std::ranges::find(list, anchor_, &fs::Entry::name);
            const std::size_t from = anchor != list.end()
                                         ? static_cast<std::size_t>(anchor - list.begin())
                                         : cursor_index_.value_or(target);
            anchor_ = list[from].name;
            selected_.clear();
            for (std::size_t i = std::min(from, target); i <= std::max(from, target); ++i) {
                selected_.insert(list[i].name);
            }
            break;
        }
        case SelectMode::keep:
            break;
        case SelectMode::toggle:
            if (selected_.erase(name) == 0) {
                selected_.insert(name);
            }
            anchor_ = name;
            break;
    }
    cursor_name_ = name;
    cursor_index_ = target;
    reveal_ = Reveal::glide;
    selection_changed();
}

void Folder::press(std::optional<std::size_t> index, SelectMode mode) {
    press_pending_ = false;
    if (index && *index < entries().size() && mode == SelectMode::replace && selected_.size() > 1 &&
        selected_.contains(entries()[*index].name)) {
        set_cursor(index, SelectMode::keep);
        press_pending_ = true;
        return;
    }
    set_cursor(index, mode);
}

void Folder::select_band(const std::vector<std::string>& base, std::span<const std::size_t> rows) {
    press_pending_ = false;
    std::unordered_set<std::string> chosen{base.begin(), base.end()};
    const std::vector<fs::Entry>& list = entries();
    for (const std::size_t row : rows) {
        if (row < list.size()) {
            chosen.insert(list[row].name);
        }
    }
    if (chosen == selected_) {
        return;
    }
    selected_ = std::move(chosen);
    selection_changed();
}

void Folder::release_press() {
    if (press_pending_) {
        press_pending_ = false;
        set_cursor(cursor_index_, SelectMode::replace);
    }
}

void Folder::move_cursor(std::ptrdiff_t delta, SelectMode mode) {
    const std::size_t count = entries().size();
    if (count == 0) {
        return;
    }
    if (!cursor_index_) {
        set_cursor(0, mode == SelectMode::keep ? SelectMode::keep : SelectMode::replace);
        return;
    }
    const auto last = static_cast<std::ptrdiff_t>(count) - 1;
    const std::ptrdiff_t target =
        std::clamp(static_cast<std::ptrdiff_t>(*cursor_index_) + delta, std::ptrdiff_t{0}, last);
    set_cursor(static_cast<std::size_t>(target), mode);
}

void Folder::expect_name(std::string name) {
    select_name(name);
    if (cursor_index_) {
        return;  // already listed
    }
    cursor_name_ = std::move(name);
    cursor_expected_ = true;
    selected_ = {cursor_name_};
    anchor_ = cursor_name_;
    selection_changed();
}

void Folder::expect_names(const std::vector<std::string>& names) {
    if (names.empty()) {
        return;
    }
    expect_name(names.front());
    // Selected by name: the ones not listed yet are selected when they arrive.
    selected_.insert(names.begin(), names.end());
    selection_changed();
}

void Folder::select_name(std::string name) {
    if (!shows_hidden() && hidden_name(name)) {
        model_.set_filter(false, name);  // asked for by name: shown all the same
    }
    cursor_expected_ = false;
    cursor_name_ = std::move(name);
    cursor_index_.reset();
    const std::vector<fs::Entry>& list = entries();
    const auto it = std::ranges::find(list, cursor_name_, &fs::Entry::name);
    if (it != list.end()) {
        cursor_index_ = static_cast<std::size_t>(it - list.begin());
    } else if (!model_.loading()) {
        cursor_name_.clear();
    }
    selected_.clear();
    if (!cursor_name_.empty()) {
        selected_.insert(cursor_name_);
    }
    anchor_ = cursor_name_;
    reveal_ = Reveal::jump;  // now, or when it lands
    selection_changed();
}

void Folder::select_all() {
    const std::vector<fs::Entry>& list = entries();
    if (list.empty()) {
        return;
    }
    for (const fs::Entry& entry : list) {
        selected_.insert(entry.name);
    }
    if (!cursor_index_) {
        cursor_index_ = 0;
        cursor_name_ = list.front().name;
        anchor_ = cursor_name_;
    }
    selection_changed();
}

void Folder::set_show_hidden(bool show, bool keep_cursor) {
    if (show == shows_hidden()) {
        return;
    }
    std::string keep;
    std::string next;  // where the cursor goes when its entry is left out
    if (const fs::Entry* const entry = cursor_entry();
        !show && entry != nullptr && entry->hidden()) {
        if (keep_cursor) {
            keep = entry->name;
        } else {
            const std::vector<fs::Entry>& list = entries();
            const auto shown = [](const fs::Entry& candidate) { return !candidate.hidden(); };
            const auto at = list.begin() + (entry - list.data());
            if (const auto after = std::find_if(at, list.end(), shown); after != list.end()) {
                next = after->name;
            } else if (const auto before =
                           std::find_if(std::make_reverse_iterator(at), list.rend(), shown);
                       before != list.rend()) {
                next = before->name;
            }
        }
    }
    model_.set_filter(show, std::move(keep));
    if (!next.empty()) {
        // The rest of the selection stays; alone, the cursor's entry is selected.
        const std::vector<fs::Entry>& list = entries();
        const auto it = std::ranges::find(list, next, &fs::Entry::name);
        set_cursor(static_cast<std::size_t>(it - list.begin()),
                   selected_.empty() ? SelectMode::replace : SelectMode::keep);
    }
}

void Folder::collapse_selection() {
    selected_.clear();
    if (cursor_index_) {
        selected_.insert(cursor_name_);
        anchor_ = cursor_name_;
    }
    selection_changed();
}

fs::MimeId Folder::mime_of(const fs::Entry& entry) const {
    if (entry.mime != fs::kUnknownMime) {
        return entry.mime;
    }
    const auto it = sniffed_.find(entry.name);
    return it == sniffed_.end() ? fs::kUnknownMime : it->second;
}

std::filesystem::path Folder::path_of(const fs::Entry& entry) const {
    if (const fs::TrashedItem* const item = model_.trashed(entry.name)) {
        return item->path;
    }
    return path() / entry.name;
}

void Folder::sniff(std::size_t first, std::size_t last) {
    const std::vector<fs::Entry>& list = entries();
    // Each entry with where it really is (the trash folder's are elsewhere).
    std::vector<std::pair<fs::Entry, std::filesystem::path>> batch;
    for (std::size_t i = first; i < std::min(last, list.size()) && batch.size() < kMaxSniffBatch;
         ++i) {
        const fs::Entry& entry = list[i];
        if (entry.mime == fs::kUnknownMime && !sniffed_.contains(entry.name) &&
            !sniffing_.contains(entry.name)) {
            sniffing_.insert(entry.name);
            batch.emplace_back(entry, path_of(entry));
        }
    }
    if (batch.empty()) {
        return;
    }
    pool_.submit(
        core::Priority::visible,
        [mime = mime_, batch](const core::CancelToken& token) {
            std::vector<std::pair<std::string, fs::MimeId>> types;
            types.reserve(batch.size());
            for (const auto& [entry, path] : batch) {
                if (token.cancelled()) {
                    break;
                }
                fs::Entry real = entry;
                real.name = path.filename().string();
                types.emplace_back(entry.name, mime->detect(path.parent_path(), real));
            }
            return types;
        },
        [this, alive = alive_](std::vector<std::pair<std::string, fs::MimeId>> types) {
            if (!*alive) {
                return;  // the folder was closed
            }
            for (auto& [name, type] : types) {
                sniffing_.erase(name);
                sniffed_[std::move(name)] = type;
            }
            redraw_();
        });
}

}  // namespace rummage::views
