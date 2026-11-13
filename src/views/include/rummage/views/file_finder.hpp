#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/walk.hpp"

// Finding files under a folder (Ctrl+F, ROADMAP 8b): every entry under it is listed once, on a
// worker, breadth first, and what is typed is matched against their paths on the workers, a new
// query cancelling the one before. The best kMaxResults are kept; with nothing typed, the first
// entries found (the nearest).
//
// Loop thread only. Must not outlive the pool.
namespace rummage::views {

struct FoundFile {
    std::string path;  // relative to the folder searched
    bool directory{false};
    double score{0.0};

    bool operator==(const FoundFile&) const = default;
};

class FileFinder {
public:
    static constexpr std::size_t kMaxResults = 200;

    // changed: results or progress changed (redraw, refill the palette).
    FileFinder(core::WorkerPool& pool, std::function<void()> changed, fs::WalkOptions options = {});
    ~FileFinder();

    FileFinder(const FileFinder&) = delete;
    FileFinder& operator=(const FileFinder&) = delete;
    FileFinder(FileFinder&&) = delete;
    FileFinder& operator=(FileFinder&&) = delete;

    // Starts listing root (dotfiles with hidden).
    void start(const std::filesystem::path& root, bool hidden);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    void set_query(const std::string& query);

    [[nodiscard]] const std::string& query() const noexcept { return query_; }

    // Best first: score, then the shorter path, then the path.
    [[nodiscard]] const std::vector<FoundFile>& results() const noexcept { return results_; }

    [[nodiscard]] bool scanning() const noexcept { return scanning_; }

    // A query is being matched.
    [[nodiscard]] bool matching() const noexcept { return matching_ > 0; }

    [[nodiscard]] std::size_t entries() const noexcept { return entries_; }

    [[nodiscard]] const fs::WalkResult& walked() const noexcept { return walked_; }

    // A line for the palette: "Searching… 12 345 items", "52 310 items", "Stopped at …".
    [[nodiscard]] std::string status() const;

    // Bytes of the paths held (the memory report).
    [[nodiscard]] std::size_t bytes() const noexcept;

private:
    using Batch = std::shared_ptr<const fs::PathBatch>;

    void match(std::vector<Batch> batches);
    void merge(std::vector<FoundFile> found);
    void fill_nearest();

    core::WorkerPool& pool_;
    std::function<void()> changed_;
    fs::WalkOptions options_;
    std::filesystem::path root_;
    std::vector<Batch> batches_;
    std::size_t entries_{0};
    bool scanning_{false};
    fs::WalkResult walked_;
    core::CancelToken scan_token_;
    std::string query_;
    std::uint64_t generation_{0};
    std::vector<core::CancelToken> match_tokens_;
    std::size_t matching_{0};
    std::vector<FoundFile> results_;
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

// The best of batches for query, at most limit, ordered as FileFinder::results(). Blocking (the
// workers'); polls token.
[[nodiscard]] std::vector<FoundFile> match_paths(
    const std::vector<std::shared_ptr<const fs::PathBatch>>& batches, const std::string& query,
    std::size_t limit, const core::CancelToken& token);

}  // namespace rummage::views
