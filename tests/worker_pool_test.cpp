#include "rummage/core/worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <numeric>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"

using rummage::core::CancelToken;
using rummage::core::EventLoop;
using rummage::core::Priority;
using rummage::core::WorkerPool;
using namespace std::chrono_literals;

namespace {

void add_watchdog(EventLoop& loop) {
    loop.add_timer(10s, 0ms, [&loop] {
        FAIL_CHECK("event loop watchdog fired");
        loop.quit(-1);
    });
}

// Occupies a worker until release(); started() returns once it is running.
class Blocker {
public:
    void submit(WorkerPool& pool) {
        pool.submit(
            Priority::visible,
            [this](const CancelToken& /*token*/) {
                started_.set_value();
                gate_.wait();
                return 0;
            },
            [](int /*result*/) {});
        started_future_.wait();
    }

    void release() { open_.set_value(); }

private:
    std::promise<void> started_;
    std::future<void> started_future_{started_.get_future()};
    std::promise<void> open_;
    std::shared_future<void> gate_{open_.get_future().share()};
};

}  // namespace

TEST_CASE("work runs on a worker, done runs on the loop thread with the result") {
    EventLoop loop;
    add_watchdog(loop);
    WorkerPool pool{loop, 2};
    std::thread::id worker_thread;
    std::thread::id done_thread;
    pool.submit(
        Priority::visible,
        [&](const CancelToken& /*token*/) {
            worker_thread = std::this_thread::get_id();
            return 41;
        },
        [&](int result) {
            done_thread = std::this_thread::get_id();
            CHECK(result == 41);
            loop.quit(0);
        });
    CHECK(loop.run() == 0);
    CHECK(worker_thread != std::this_thread::get_id());
    CHECK(done_thread == std::this_thread::get_id());
}

TEST_CASE("queued work runs by priority: visible, nearby, prefetch") {
    EventLoop loop;
    add_watchdog(loop);
    Blocker blocker;  // before the pool: it must outlive the job that waits on it
    WorkerPool pool{loop, 1};
    blocker.submit(pool);  // the only worker is busy: the next three stay queued

    std::mutex mutex;
    std::vector<Priority> order;
    int delivered = 0;
    for (const Priority priority : {Priority::prefetch, Priority::nearby, Priority::visible}) {
        pool.submit(
            priority,
            [&, priority](const CancelToken& /*token*/) {
                const std::scoped_lock lock{mutex};
                order.push_back(priority);
                return 0;
            },
            [&](int /*result*/) {
                if (++delivered == 3) {
                    loop.quit(0);
                }
            });
    }
    blocker.release();
    CHECK(loop.run() == 0);
    CHECK(order == std::vector{Priority::visible, Priority::nearby, Priority::prefetch});
}

TEST_CASE("a job cancelled while queued never runs and never delivers") {
    EventLoop loop;
    add_watchdog(loop);
    Blocker blocker;  // before the pool: it must outlive the job that waits on it
    WorkerPool pool{loop, 1};
    blocker.submit(pool);

    std::atomic<bool> ran{false};
    bool delivered = false;
    const CancelToken token = pool.submit(
        Priority::visible,
        [&](const CancelToken& /*token*/) {
            ran = true;
            return 0;
        },
        [&](int /*result*/) { delivered = true; });
    token.cancel();
    // Queued after the cancelled job: when this one delivers, the other had its turn.
    pool.submit(
        Priority::visible, [](const CancelToken& /*token*/) { return 0; },
        [&](int /*result*/) { loop.quit(0); });
    blocker.release();

    CHECK(loop.run() == 0);
    CHECK_FALSE(ran.load());
    CHECK_FALSE(delivered);
}

TEST_CASE("a job cancelled while running finishes but does not deliver") {
    EventLoop loop;
    add_watchdog(loop);
    WorkerPool pool{loop, 1};
    std::promise<void> running;
    std::promise<void> proceed;
    auto proceed_future = proceed.get_future().share();
    bool delivered = false;

    const CancelToken token = pool.submit(
        Priority::visible,
        [&](const CancelToken& job_token) {
            running.set_value();
            proceed_future.wait();
            CHECK(job_token.cancelled());  // the work can see it and stop early
            return 0;
        },
        [&](int /*result*/) { delivered = true; });
    running.get_future().wait();
    token.cancel();
    proceed.set_value();
    pool.submit(
        Priority::visible, [](const CancelToken& /*token*/) { return 0; },
        [&](int /*result*/) { loop.quit(0); });

    CHECK(loop.run() == 0);
    CHECK_FALSE(delivered);
}

TEST_CASE("destroying the pool drops queued work and suppresses pending deliveries") {
    EventLoop loop;
    add_watchdog(loop);
    std::atomic<int> ran{0};
    int delivered = 0;
    Blocker blocker;
    std::thread releaser;
    {
        WorkerPool pool{loop, 1};
        blocker.submit(pool);
        for (int i = 0; i < 100; ++i) {
            pool.submit(
                Priority::nearby,
                [&](const CancelToken& /*token*/) {
                    ++ran;
                    return 0;
                },
                [&](int /*result*/) { ++delivered; });
        }
        // The destructor joins the busy worker: release it from another thread meanwhile.
        releaser = std::thread{[&] {
            std::this_thread::sleep_for(20ms);
            blocker.release();
        }};
    }
    releaser.join();
    loop.post([&] { loop.quit(0); });
    CHECK(loop.run() == 0);
    CHECK(ran.load() == 0);
    CHECK(delivered == 0);
}

TEST_CASE("many jobs on many threads: every result delivered exactly once") {
    EventLoop loop;
    add_watchdog(loop);
    WorkerPool pool{loop};
    CHECK(pool.thread_count() >= 2);

    constexpr int jobs = 2000;
    std::vector<int> seen(jobs, 0);
    int delivered = 0;
    for (int i = 0; i < jobs; ++i) {
        const auto priority = static_cast<Priority>(i % 3);
        pool.submit(
            priority, [i](const CancelToken& /*token*/) { return i * 2; },
            [&, i](int result) {
                CHECK(result == i * 2);
                ++seen[static_cast<std::size_t>(i)];
                if (++delivered == jobs) {
                    loop.quit(0);
                }
            });
    }
    CHECK(loop.run() == 0);
    for (const int count : seen) {
        CHECK(count == 1);
    }
}

TEST_CASE("submit_streaming: partial results in order, then done; cancel stops them") {
    EventLoop loop;
    add_watchdog(loop);
    WorkerPool pool{loop, 2};

    std::vector<int> partials;
    bool done_seen_all = false;
    pool.submit_streaming(
        Priority::visible,
        [](const CancelToken& /*token*/, auto& emit) {
            for (int i = 0; i < 100; ++i) {
                emit(i);
            }
            return 100;
        },
        [&](int value) { partials.push_back(value); },
        [&](int total) {
            done_seen_all = std::cmp_equal(partials.size(), total);
            loop.quit(0);
        });
    loop.run();
    CHECK(done_seen_all);
    std::vector<int> expected(100);
    std::iota(expected.begin(), expected.end(), 0);
    CHECK(partials == expected);

    // Cancelled from the first partial: nothing else arrives, done included.
    int received = 0;
    bool done_called = false;
    CancelToken token;
    std::promise<void> emitted_all;
    token = pool.submit_streaming(
        Priority::visible,
        [&emitted_all](const CancelToken& /*token*/, auto& emit) {
            for (int i = 0; i < 10; ++i) {
                emit(i);
            }
            emitted_all.set_value();
            return 0;
        },
        [&](int /*value*/) {
            ++received;
            token.cancel();
        },
        [&](int /*total*/) { done_called = true; });
    emitted_all.get_future().wait();
    loop.add_timer(50ms, 0ms, [&loop] { loop.quit(0); });
    loop.run();
    CHECK(received == 1);
    CHECK_FALSE(done_called);
}
