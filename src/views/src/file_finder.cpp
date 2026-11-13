#include "rummage/views/file_finder.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "rummage/views/fuzzy.hpp"
#include "rummage/core/i18n.hpp"

namespace rummage::views {
namespace {

// The order of results: score, then the shorter path, then the path.
bool better(const FoundFile& a, const FoundFile& b) {
    if (a.score != b.score) {
        return a.score > b.score;
    }
    if (a.path.size() != b.path.size()) {
        return a.path.size() < b.path.size();
    }
    return a.path < b.path;
}

std::string count(std::size_t n) {
    // "12 345": groups of three, as the status bar writes them.
    std::string digits = std::to_string(n);
    for (std::ptrdiff_t at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), " ");
    }
    return digits;
}

}  // namespace

std::vector<FoundFile> match_paths(const std::vector<std::shared_ptr<const fs::PathBatch>>& batches,
                                   const std::string& query, std::size_t limit,
                                   const core::CancelToken& token) {
    FuzzyQuery fuzzy{query};
    std::vector<FoundFile> best;  // a heap: the worst kept on top
    const auto worse = [](const FoundFile& a, const FoundFile& b) { return better(a, b); };
    for (const std::shared_ptr<const fs::PathBatch>& batch : batches) {
        if (token.cancelled()) {
            return {};
        }
        for (std::size_t i = 0; i < batch->size(); ++i) {
            const std::string_view path = batch->path(i);
            const std::optional<double> score = fuzzy.path_score(path);
            if (!score) {
                continue;
            }
            if (best.size() == limit) {
                const FoundFile& last = best.front();
                if (*score < last.score ||
                    (*score == last.score && path.size() >= last.path.size())) {
                    continue;
                }
                std::ranges::pop_heap(best, worse);
                best.pop_back();
            }
            best.push_back(
                {.path = std::string{path}, .directory = batch->directory(i), .score = *score});
            std::ranges::push_heap(best, worse);
        }
    }
    std::ranges::sort(best, better);
    return best;
}

FileFinder::FileFinder(core::WorkerPool& pool, std::function<void()> changed,
                       fs::WalkOptions options)
    : pool_{pool}, changed_{std::move(changed)}, options_{options} {}

FileFinder::~FileFinder() {
    *alive_ = false;
    scan_token_.cancel();
    for (const core::CancelToken& token : match_tokens_) {
        token.cancel();
    }
}

void FileFinder::start(const std::filesystem::path& root, bool hidden) {
    scan_token_.cancel();
    root_ = root;
    batches_.clear();
    entries_ = 0;
    walked_ = {};
    results_.clear();
    scanning_ = true;
    fs::WalkOptions options = options_;
    options.hidden = hidden;
    scan_token_ = pool_.submit_streaming(
        core::Priority::visible,
        [root, options](const core::CancelToken& token, auto emit) {
            return fs::walk(root, options, token, [&emit](fs::PathBatch batch) {
                emit(std::make_shared<const fs::PathBatch>(std::move(batch)));
            });
        },
        [this, alive = alive_](Batch batch) {
            if (!*alive) {
                return;
            }
            entries_ += batch->size();
            batches_.push_back(batch);
            if (query_.empty()) {
                fill_nearest();
            } else {
                match({std::move(batch)});  // the new entries for the query typed
            }
            changed_();
        },
        [this, alive = alive_](const fs::WalkResult& result) {
            if (!*alive) {
                return;
            }
            scanning_ = false;
            walked_ = result;
            changed_();
        });
}

void FileFinder::set_query(const std::string& query) {
    if (query == query_) {
        return;
    }
    query_ = query;
    ++generation_;
    for (const core::CancelToken& token : match_tokens_) {
        token.cancel();
    }
    match_tokens_.clear();
    matching_ = 0;
    results_.clear();
    if (FuzzyQuery{query_}.empty()) {
        fill_nearest();
    } else if (!batches_.empty()) {
        // Spread over the workers: a share of the batches each.
        const std::size_t shares =
            std::clamp<std::size_t>(pool_.thread_count(), 1, batches_.size());
        std::vector<std::vector<Batch>> parts(shares);
        for (std::size_t i = 0; i < batches_.size(); ++i) {
            parts[i % shares].push_back(batches_[i]);
        }
        for (std::vector<Batch>& part : parts) {
            match(std::move(part));
        }
    }
    changed_();
}

void FileFinder::match(std::vector<Batch> batches) {
    ++matching_;
    match_tokens_.push_back(pool_.submit(
        core::Priority::visible,
        [batches = std::move(batches), query = query_](const core::CancelToken& token) {
            return match_paths(batches, query, kMaxResults, token);
        },
        [this, alive = alive_, generation = generation_](const std::vector<FoundFile>& found) {
            if (!*alive || generation != generation_) {
                return;
            }
            --matching_;
            merge(found);
            changed_();
        }));
}

void FileFinder::merge(std::vector<FoundFile> found) {
    if (found.empty()) {
        return;
    }
    std::vector<FoundFile> merged;
    merged.reserve(std::min(results_.size() + found.size(), kMaxResults));
    std::ranges::merge(std::move(results_), std::move(found), std::back_inserter(merged), better);
    if (merged.size() > kMaxResults) {
        merged.resize(kMaxResults);
    }
    results_ = std::move(merged);
}

void FileFinder::fill_nearest() {
    if (!FuzzyQuery{query_}.empty()) {
        return;
    }
    results_.clear();
    for (const Batch& batch : batches_) {
        for (std::size_t i = 0; i < batch->size() && results_.size() < kMaxResults; ++i) {
            results_.push_back(
                {.path = std::string{batch->path(i)}, .directory = batch->directory(i)});
        }
        if (results_.size() == kMaxResults) {
            break;
        }
    }
}

std::string FileFinder::status() const {
    if (scanning_) {
        return i18n::trf("Searching… {} items", count(entries_));
    }
    if (walked_.truncated) {
        return i18n::trf("Stopped at {} items", count(entries_));
    }
    return i18n::trf("{} items", count(entries_));
}

std::size_t FileFinder::bytes() const noexcept {
    std::size_t total = 0;
    for (const Batch& batch : batches_) {
        total += batch->bytes();
    }
    return total;
}

}  // namespace rummage::views
