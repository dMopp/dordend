#include "rummage/fs/directory_model.hpp"

#include <algorithm>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "rummage/core/log.hpp"

namespace rummage::fs {
namespace {

ModelChange reset_change() {
    ModelChange change;
    change.reset = true;
    return change;
}

ModelChange loaded_change() {
    ModelChange change;
    change.loaded = true;
    return change;
}

}  // namespace

DirectoryModel::DirectoryModel(core::EventLoop& loop, core::WorkerPool& pool,
                               DirectoryWatcher& watcher, std::shared_ptr<const MimeDatabase> mime,
                               std::chrono::milliseconds settle)
    : loop_{loop}, pool_{pool}, watcher_{watcher}, settle_{settle} {
    options_.mime = std::move(mime);
}

DirectoryModel::~DirectoryModel() { stop(); }

void DirectoryModel::open(std::filesystem::path path) {
    stop();
    path_ = std::move(path);
    listing_.clear();
    names_.clear();
    error_.clear();
    notify(reset_change());

    if (fs::is_trash(path_)) {
        trash_ = true;
        loading_ = true;
        load_trash();
        return;
    }
    std::error_code error;
    watch_ = watcher_.watch(
        path_, [this](const std::vector<DirectoryEvent>& events) { on_events(events); }, error);
    if (error == std::errc::no_space_on_device) {
        // The per-user inotify watch limit: show the directory anyway, without live updates.
        log::warn("{}: cannot watch for changes (inotify watch limit reached)", path_.string());
    } else if (error) {
        fail(error);
        return;
    }
    start_load();
}

void DirectoryModel::set_order(SortOrder order) {
    listing_.set_order(order);
    notify(reset_change());
}

void DirectoryModel::refresh_entry(const std::string& name) {
    dirty_.insert(name);
    if (!loading_)
        schedule_refresh();
}

void DirectoryModel::set_filter(bool show_hidden, std::string keep) {
    Listing::Moved moved = listing_.set_filter(show_hidden, std::move(keep));
    if (moved.hidden.empty() && moved.shown.empty()) {
        return;
    }
    ModelChange change;
    change.removed = std::move(moved.hidden);
    change.added = std::move(moved.shown);
    notify(change);
}

void DirectoryModel::set_predicate(Listing::Predicate predicate) {
    auto moved = listing_.set_predicate(std::move(predicate));
    if (moved.hidden.empty() && moved.shown.empty()) {
        return;
    }
    ModelChange change;
    change.removed = std::move(moved.hidden);
    change.added = std::move(moved.shown);
    notify(change);
}

void DirectoryModel::start_load() {
    loading_ = true;
    load_ = load_directory(
        pool_, path_,
        [this](std::vector<Entry> chunk) {
            ModelChange change;
            change.added.reserve(chunk.size());
            for (const Entry& entry : chunk) {
                names_.insert(entry.name);
                change.added.push_back(entry.name);
            }
            listing_.add(std::move(chunk));
            notify(change);
        },
        [this](const ReadResult& result) {
            loading_ = false;
            if (result.error) {
                fail(result.error);
                return;
            }
            notify(loaded_change());
            if (!dirty_.empty()) {
                schedule_refresh();  // changes that happened while loading
            }
        },
        options_);
}

void DirectoryModel::stop() {
    load_.cancel();
    refresh_job_.cancel();
    refresh_running_ = false;
    loop_.remove(settle_timer_);
    settle_timer_ = 0;
    watcher_.unwatch(watch_);
    watch_ = 0;
    loading_ = false;
    dirty_.clear();
    moved_from_.clear();
    renames_.clear();
    for (const auto& [dir, id] : trash_watches_) {
        watcher_.unwatch(id);
    }
    trash_watches_.clear();
    trashed_.clear();
    trash_ = false;
    trash_again_ = false;
}

const TrashedItem* DirectoryModel::trashed(const std::string& name) const {
    const auto it = trashed_.find(name);
    return it == trashed_.end() ? nullptr : &it->second;
}

struct DirectoryModel::TrashListing {
    std::vector<TrashedItem> items;
    std::vector<Entry> entries;  // items[i]'s, named as listed
    std::vector<std::filesystem::path> info_dirs;
};

void DirectoryModel::load_trash() {
    refresh_running_ = true;
    refresh_job_ = pool_.submit(
        core::Priority::visible,
        [sources = trash_sources(), options = options_](const core::CancelToken& /*token*/) {
            // The home trash is made when missing (the specification asks for it), so it can be
            // watched from the start.
            if (!sources.home.empty()) {
                std::error_code ignored;
                std::filesystem::create_directories(sources.home / "files", ignored);
                std::filesystem::create_directories(sources.home / "info", ignored);
            }
            TrashListing listing;
            for (const TrashDir& trash : existing_trashes(sources)) {
                listing.info_dirs.push_back(trash.path / "info");
            }
            std::vector<TrashedItem> items = list_trash(sources);
            for (TrashedItem& item : items) {
                std::vector<std::optional<Entry>> read =
                    read_entries(item.path.parent_path(), {item.path.filename().string()}, options);
                if (read.empty() || !read.front()) {
                    continue;  // gone meanwhile
                }
                Entry entry = std::move(*read.front());
                entry.name = item.name;
                listing.entries.push_back(std::move(entry));
                listing.items.push_back(std::move(item));
            }
            return listing;
        },
        [this](TrashListing listing) {
            refresh_running_ = false;
            apply_trash(std::move(listing));
            if (std::exchange(trash_again_, false)) {
                load_trash();
            }
        });
}

void DirectoryModel::apply_trash(TrashListing listing) {
    // Watch the info/ folders: an item comes or goes with its .trashinfo.
    std::unordered_map<std::string, DirectoryWatcher::WatchId> watches;
    for (const std::filesystem::path& dir : listing.info_dirs) {
        const std::string key = dir.string();
        if (const auto it = trash_watches_.find(key); it != trash_watches_.end()) {
            watches.emplace(key, it->second);
            trash_watches_.erase(it);
            continue;
        }
        std::error_code error;
        const DirectoryWatcher::WatchId id = watcher_.watch(
            dir,
            [this](const std::vector<DirectoryEvent>& /*events*/) {
                if (refresh_running_) {
                    trash_again_ = true;
                    return;
                }
                if (settle_timer_ == 0) {
                    settle_timer_ = loop_.add_timer(settle_, std::chrono::milliseconds{0}, [this] {
                        settle_timer_ = 0;
                        load_trash();
                    });
                }
            },
            error);
        if (id != 0) {
            watches.emplace(key, id);
        }
    }
    for (const auto& [dir, id] : trash_watches_) {
        watcher_.unwatch(id);  // a trash that went away (a disk unmounted)
    }
    trash_watches_ = std::move(watches);

    // What changed, by name.
    std::unordered_map<std::string_view, const Entry*> listed;
    listed.reserve(listing_.entries().size() + listing_.hidden().size());
    for (const auto* list : {&listing_.entries(), &listing_.hidden()}) {
        for (const Entry& entry : *list) {
            listed.emplace(entry.name, &entry);
        }
    }
    std::unordered_map<std::string, TrashedItem> items;
    ModelChange change;
    std::vector<Entry> present;
    for (std::size_t i = 0; i < listing.items.size(); ++i) {
        Entry& entry = listing.entries[i];
        const std::string name = entry.name;
        const auto before = listed.find(name);
        const TrashedItem* const was = trashed(name);
        if (before == listed.end()) {
            change.added.push_back(name);
            present.push_back(std::move(entry));
        } else if (was == nullptr || was->path != listing.items[i].path ||
                   before->second->size != entry.size ||
                   before->second->modified_ns != entry.modified_ns ||
                   before->second->kind != entry.kind) {
            change.updated.push_back(name);
            present.push_back(std::move(entry));
        }
        items.emplace(name, std::move(listing.items[i]));
    }
    std::unordered_set<std::string> gone;
    for (const auto* list : {&listing_.entries(), &listing_.hidden()}) {
        for (const Entry& entry : *list) {
            if (!items.contains(entry.name)) {
                gone.insert(entry.name);
                change.removed.push_back(entry.name);
            }
        }
    }
    listing_.replace(gone, std::move(present));
    trashed_ = std::move(items);
    names_.clear();
    for (const auto* list : {&listing_.entries(), &listing_.hidden()}) {
        for (const Entry& entry : *list) {
            names_.insert(entry.name);
        }
    }
    const bool first = loading_;
    loading_ = false;
    if (!change.added.empty() || !change.removed.empty() || !change.updated.empty()) {
        notify(change);
    }
    if (first) {
        notify(loaded_change());
    }
}

void DirectoryModel::on_events(const std::vector<DirectoryEvent>& events) {
    using Kind = DirectoryEvent::Kind;
    for (const DirectoryEvent& event : events) {
        switch (event.kind) {
            case Kind::directory_gone:
                fail(std::make_error_code(std::errc::no_such_file_or_directory));
                return;
            case Kind::overflow:
                // Events were lost: read the whole directory again.
                load_.cancel();
                refresh_job_.cancel();
                refresh_running_ = false;
                loop_.remove(settle_timer_);
                settle_timer_ = 0;
                dirty_.clear();
                moved_from_.clear();
                renames_.clear();
                listing_.clear();
                names_.clear();
                notify(reset_change());
                start_load();
                return;
            case Kind::moved_from:
                moved_from_[event.cookie] = event.name;
                dirty_.insert(event.name);
                break;
            case Kind::moved_to:
                if (const auto it = moved_from_.find(event.cookie); it != moved_from_.end()) {
                    renames_.emplace_back(it->second, event.name);
                    moved_from_.erase(it);
                }
                dirty_.insert(event.name);
                break;
            case Kind::created:
            case Kind::deleted:
            case Kind::changed:
                dirty_.insert(event.name);
                break;
        }
    }
    if (!loading_) {
        schedule_refresh();
    }
}

void DirectoryModel::schedule_refresh() {
    if (settle_timer_ != 0 || refresh_running_) {
        return;  // already coming; a running refresh reschedules when it lands
    }
    settle_timer_ = loop_.add_timer(settle_, std::chrono::milliseconds{0}, [this] {
        settle_timer_ = 0;
        refresh();
    });
}

void DirectoryModel::refresh() {
    if (dirty_.empty()) {
        return;
    }
    std::vector<std::string> names(dirty_.begin(), dirty_.end());
    std::vector<std::pair<std::string, std::string>> renames = std::move(renames_);
    dirty_.clear();
    renames_.clear();
    moved_from_.clear();  // a move away whose pair never came: just a removal
    refresh_running_ = true;
    refresh_job_ = pool_.submit(
        core::Priority::visible,
        [path = path_, names, options = options_](const core::CancelToken& /*token*/) {
            return read_entries(path, names, options);
        },
        [this, names, renames = std::move(renames)](std::vector<std::optional<Entry>> entries) {
            refresh_running_ = false;
            apply(names, std::move(entries), renames);
            if (!dirty_.empty()) {
                schedule_refresh();
            }
        });
}

void DirectoryModel::apply(const std::vector<std::string>& names,
                           std::vector<std::optional<Entry>> entries,
                           const std::vector<std::pair<std::string, std::string>>& renames) {
    ModelChange change;
    std::unordered_set<std::string> gone;
    std::vector<Entry> present;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        if (std::optional<Entry>& entry = entries[i]; entry.has_value()) {
            (names_.contains(name) ? change.updated : change.added).push_back(name);
            names_.insert(name);
            present.push_back(std::move(entry.value()));
        } else if (names_.erase(name) > 0) {
            change.removed.push_back(name);
            gone.insert(name);
        }
    }
    // A rename is a removal of the old name and an addition of the new one in the same batch.
    for (const auto& [from, to] : renames) {
        const auto removed = std::ranges::find(change.removed, from);
        const auto added = std::ranges::find(change.added, to);
        if (removed != change.removed.end() && added != change.added.end()) {
            change.removed.erase(removed);
            change.added.erase(added);
            change.renamed.emplace_back(from, to);
        }
    }
    listing_.replace(gone, std::move(present));
    if (!change.added.empty() || !change.removed.empty() || !change.updated.empty() ||
        !change.renamed.empty()) {
        notify(change);
    }
}

void DirectoryModel::notify(const ModelChange& change) {
    ++revision_;
    if (change_handler_) {
        change_handler_(change);
    }
}

void DirectoryModel::fail(std::error_code error) {
    stop();
    error_ = error;
    listing_.clear();
    names_.clear();
    notify(reset_change());
}

}  // namespace rummage::fs
