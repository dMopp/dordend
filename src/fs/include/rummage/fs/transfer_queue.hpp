#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>

#include "rummage/core/event_loop.hpp"
#include "rummage/fs/transfer.hpp"

namespace rummage::fs {

// Runs transfers one at a time, in order, on a thread of its own (a long copy does not hold a
// worker that thumbnails need). What happens reaches the loop thread: progress at most every
// kProgressInterval, each conflict as a question (the transfer waits for the answer), and the
// result.
//
// Callbacks run on the loop thread. Destroying the queue cancels the transfer running (a question
// waiting is answered "cancel"), drops the others and joins the thread.
class TransferQueue {
public:
    static constexpr std::chrono::milliseconds kProgressInterval{100};

    using Progress =
        std::function<void(const TransferRequest& request, const TransferProgress& progress)>;
    // answer(resolution) may be called later, once; until then the transfer waits.
    using Ask =
        std::function<void(const Conflict& conflict, std::function<void(Resolution)> answer)>;
    using Done = std::function<void(const TransferRequest& request, const TransferResult& result)>;

    TransferQueue(core::EventLoop& loop, Progress progress, Ask ask, Done done);
    ~TransferQueue();

    TransferQueue(const TransferQueue&) = delete;
    TransferQueue& operator=(const TransferQueue&) = delete;
    TransferQueue(TransferQueue&&) = delete;
    TransferQueue& operator=(TransferQueue&&) = delete;

    void submit(TransferRequest request);

    // Stops the transfer running (what is done stays done); the queued ones go on.
    void cancel();

    // Transfers submitted and not done yet, the one running included (loop thread).
    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }

private:
    struct Shared;

    void work();

    core::EventLoop& loop_;
    Progress progress_;
    Ask ask_;
    Done done_;
    std::size_t pending_{0};
    std::shared_ptr<Shared> shared_;  // what the thread and the loop both touch
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    std::thread thread_;  // last: started once everything else is ready
};

}  // namespace rummage::fs
