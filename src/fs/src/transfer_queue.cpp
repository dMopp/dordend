#include "rummage/fs/transfer_queue.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "rummage/fs/erase.hpp"
#include "rummage/fs/trash.hpp"

namespace rummage::fs {

struct TransferQueue::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<TransferRequest> jobs;
    bool stopping{false};
    core::CancelToken current;  // the running transfer's
    // The question the transfer waits on: its number, and the answer once given.
    std::uint64_t question{0};
    bool asking{false};
    std::optional<Resolution> answer;
};

TransferQueue::TransferQueue(core::EventLoop& loop, Progress progress, Ask ask, Done done)
    : loop_{loop},
      progress_{std::move(progress)},
      ask_{std::move(ask)},
      done_{std::move(done)},
      shared_{std::make_shared<Shared>()},
      thread_{[this] { work(); }} {}

TransferQueue::~TransferQueue() {
    *alive_ = false;  // posted callbacks still on their way find out here
    {
        const std::scoped_lock lock{shared_->mutex};
        shared_->stopping = true;
        shared_->jobs.clear();
        shared_->current.cancel();
    }
    shared_->wake.notify_all();
    thread_.join();
}

void TransferQueue::submit(TransferRequest request) {
    ++pending_;
    {
        const std::scoped_lock lock{shared_->mutex};
        shared_->jobs.push_back(std::move(request));
    }
    shared_->wake.notify_all();
}

void TransferQueue::cancel() {
    {
        const std::scoped_lock lock{shared_->mutex};
        shared_->current.cancel();
        if (shared_->asking) {
            shared_->answer = Resolution::cancel;
            shared_->asking = false;
        }
    }
    shared_->wake.notify_all();
}

void TransferQueue::work() {
    const std::shared_ptr<Shared> shared = shared_;
    for (;;) {
        core::CancelToken token;
        std::shared_ptr<const TransferRequest> request;
        {
            std::unique_lock lock{shared->mutex};
            shared->wake.wait(lock, [&] { return shared->stopping || !shared->jobs.empty(); });
            if (shared->stopping) {
                return;
            }
            request = std::make_shared<const TransferRequest>(std::move(shared->jobs.front()));
            shared->jobs.pop_front();
            shared->current = token;
        }

        auto last = std::chrono::steady_clock::now() - kProgressInterval;
        TransferCallbacks callbacks;
        callbacks.progress = [&](const TransferProgress& progress) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last < kProgressInterval) {
                return;  // the loop thread redraws at most this often
            }
            last = now;
            loop_.post([this, alive = alive_, request, progress] {
                if (*alive && progress_) {
                    progress_(*request, progress);
                }
            });
        };
        callbacks.resolve = [&](const Conflict& conflict) {
            std::uint64_t question = 0;
            {
                const std::scoped_lock lock{shared->mutex};
                question = ++shared->question;
                shared->asking = true;
                shared->answer.reset();
            }
            loop_.post([this, alive = alive_, shared, conflict, question] {
                if (!*alive) {
                    return;
                }
                ask_(conflict, [shared, question](Resolution resolution) {
                    {
                        const std::scoped_lock lock{shared->mutex};
                        if (!shared->asking || shared->question != question) {
                            return;  // answered already, or cancelled
                        }
                        shared->answer = resolution;
                        shared->asking = false;
                    }
                    shared->wake.notify_all();
                });
            });
            std::unique_lock lock{shared->mutex};
            shared->wake.wait(lock, [&] { return shared->answer.has_value() || shared->stopping; });
            if (shared->stopping) {
                shared->asking = false;
                return Resolution::cancel;
            }
            const Resolution resolution = *shared->answer;
            shared->answer.reset();
            return resolution;
        };

        TransferResult result;
        switch (request->kind) {
            case TransferKind::trash:
                result = run_trash(*request, token, callbacks);
                break;
            case TransferKind::restore:
                result = run_restore(*request, token, callbacks);
                break;
            case TransferKind::erase:
            case TransferKind::empty_trash:
                result = run_erase(*request, token, callbacks);
                break;
            case TransferKind::copy:
            case TransferKind::move:
                result = run_transfer(*request, token, callbacks);
                break;
        }
        loop_.post([this, alive = alive_, request, result = std::move(result)] {
            if (!*alive) {
                return;
            }
            --pending_;
            if (done_) {
                done_(*request, result);
            }
        });
    }
}

}  // namespace rummage::fs
