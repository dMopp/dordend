#include "rummage/core/worker_pool.hpp"

#include <algorithm>

#include "rummage/core/log.hpp"

namespace rummage::core {

WorkerPool::WorkerPool(EventLoop& loop, unsigned threads) : loop_{loop} {
    if (threads == 0) {
        threads = std::max(2U, std::thread::hardware_concurrency() / 2);
    }
    threads_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        threads_.emplace_back([this] { worker_main(); });
    }
    log::debug("worker pool: {} threads", threads);
}

WorkerPool::~WorkerPool() {
    alive_->store(false);
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
        for (auto& queue : queues_) {
            for (const Job& job : queue) {
                job.token.cancel();
            }
            queue.clear();
        }
    }
    wake_.notify_all();
    for (std::thread& thread : threads_) {
        thread.join();
    }
}

void WorkerPool::enqueue(Priority priority, CancelToken token, std::function<void()> run) {
    {
        const std::scoped_lock lock{mutex_};
        queues_.at(static_cast<std::size_t>(priority))
            .push_back({.token = std::move(token), .run = std::move(run)});
    }
    wake_.notify_one();
}

void WorkerPool::worker_main() {
    while (true) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            wake_.wait(lock, [this] {
                return stopping_ || std::ranges::any_of(
                                        queues_, [](const auto& queue) { return !queue.empty(); });
            });
            if (stopping_) {
                return;
            }
            for (auto& queue : queues_) {  // highest priority first
                if (!queue.empty()) {
                    job = std::move(queue.front());
                    queue.pop_front();
                    break;
                }
            }
        }
        job.run();
    }
}

}  // namespace rummage::core
