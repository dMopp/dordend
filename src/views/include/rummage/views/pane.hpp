#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/directory_watcher.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/ui/seat.hpp"
#include "rummage/ui/text_field.hpp"
#include "rummage/views/column_view.hpp"
#include "rummage/views/details_view.hpp"
#include "rummage/views/grid_view.hpp"
#include "rummage/views/info_pane.hpp"
#include "rummage/views/thumbnails.hpp"

namespace rummage::views {

// The three ways of showing a folder.
enum class ViewMode : std::uint8_t { columns, grid, details };

// A folder opened, as the history keeps it: where the cursor was when it was left.
struct HistoryStep {
    std::filesystem::path folder;
    std::string cursor;
};

// One tab's browsing state. A Browser owns two independent tab groups, with one visible
// tab in each while split. Folder models, history, filters and view settings belong here;
// the browser shares its sidebar, preview, dialogs, thumbnail cache, transfers and undo.
struct Pane {
    Pane(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
         const std::shared_ptr<const fs::MimeDatabase>& mime, const std::function<void()>& redraw,
         const std::function<void(ui::CursorShape)>& set_cursor, Thumbnails* thumbnails,
         InfoPane* info_pane);

    ColumnView columns;  // before grid: grid folders may report to it
    GridView grid;
    DetailsView details;
    ui::TextField filter_field;
    fs::Listing::Predicate entry_filter;
    bool show_hidden{false};
    ViewMode mode{ViewMode::columns};
    std::vector<HistoryStep> history;
    std::size_t here{0};  // where history is now
};

}  // namespace rummage::views
