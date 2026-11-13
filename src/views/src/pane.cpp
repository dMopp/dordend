#include "rummage/views/pane.hpp"

namespace rummage::views {

Pane::Pane(core::EventLoop& loop, core::WorkerPool& pool, fs::DirectoryWatcher& watcher,
           const std::shared_ptr<const fs::MimeDatabase>& mime, const std::function<void()>& redraw,
           const std::function<void(ui::CursorShape)>& set_cursor, Thumbnails* thumbnails,
           InfoPane* info_pane)
    : columns{loop, pool, watcher, mime, redraw, set_cursor, info_pane},
      grid{loop, pool, watcher, mime, redraw, set_cursor, thumbnails, info_pane},
      details{loop, pool, watcher, mime, redraw, set_cursor, info_pane} {}

}  // namespace rummage::views
