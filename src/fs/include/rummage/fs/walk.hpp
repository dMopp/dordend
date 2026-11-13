#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/worker_pool.hpp"

// Every entry under a folder, for finding files (ROADMAP 8b).
namespace rummage::fs {

// A batch of entries found, as paths relative to the folder walked ("Monte Rosa/_7230017.ORF"),
// kept together in one buffer: a few hundred thousand paths cost a few blocks, not a string each.
class PathBatch {
public:
    void add(std::string_view relative, bool directory);

    [[nodiscard]] std::size_t size() const noexcept { return ends_.size(); }

    [[nodiscard]] bool empty() const noexcept { return ends_.empty(); }

    [[nodiscard]] std::string_view path(std::size_t index) const noexcept;

    [[nodiscard]] bool directory(std::size_t index) const noexcept {
        return directories_[index] != 0;
    }

    // Bytes held (the memory report).
    [[nodiscard]] std::size_t bytes() const noexcept;

    // Gives back what the buffers reserved beyond their contents (once full).
    void shrink_to_fit();

private:
    std::string text_;
    std::vector<std::uint32_t> ends_;
    std::vector<std::uint8_t> directories_;
};

struct WalkOptions {
    bool hidden{false};  // dotfiles and the folders they name
    // Stops there (truncated): about 0.5 s and 20 MB of paths (measured on 273 000 entries,
    // docs/benchmarks.md).
    std::size_t max_entries{300'000};
    std::size_t max_depth{64};  // folders deeper are not entered
    std::size_t batch{4096};    // entries handed over at a time
};

struct WalkResult {
    std::size_t entries{0};
    std::size_t unreadable{0};  // folders that could not be listed (permissions)
    bool truncated{false};      // max_entries reached
    bool cancelled{false};
};

// Walks root breadth first (the nearest entries come first), handing batches to emit as they
// fill. Symbolic links are listed but never followed; folders on another file system (a disk
// mounted inside, /proc under /) are listed but not entered. Polls token between folders and
// stops when it is cancelled. Blocking: call it from a worker.
WalkResult walk(const std::filesystem::path& root, const WalkOptions& options,
                const core::CancelToken& token, const std::function<void(PathBatch)>& emit);

}  // namespace rummage::fs
