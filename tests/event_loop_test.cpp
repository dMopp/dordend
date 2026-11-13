#include "rummage/core/event_loop.hpp"

#include <array>
#include <chrono>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <doctest/doctest.h>

using rummage::core::EventLoop;
using namespace std::chrono_literals;

namespace {

// Fails the test instead of hanging when a loop never quits.
void add_watchdog(EventLoop& loop) {
    loop.add_timer(5s, 0ms, [&loop] {
        FAIL_CHECK("event loop watchdog fired");
        loop.quit(-1);
    });
}

struct Pipe {
    std::array<int, 2> fds{-1, -1};

    Pipe() { REQUIRE(pipe(fds.data()) == 0); }

    ~Pipe() {
        close(fds[0]);
        close(fds[1]);
    }

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    Pipe(Pipe&&) = delete;
    Pipe& operator=(Pipe&&) = delete;

    void write_byte() const { REQUIRE(write(fds[1], "x", 1) == 1); }

    void read_byte() const {
        char byte = 0;
        REQUIRE(read(fds[0], &byte, 1) == 1);
    }
};

}  // namespace

TEST_CASE("post from another thread runs on the loop thread, quit sets the exit code") {
    EventLoop loop;
    add_watchdog(loop);
    std::thread::id ran_on;
    std::thread poster{[&] {
        loop.post([&] {
            ran_on = std::this_thread::get_id();
            loop.quit(7);
        });
    }};
    CHECK(loop.run() == 7);
    poster.join();
    CHECK(ran_on == std::this_thread::get_id());
}

TEST_CASE("posted functions run in posting order") {
    EventLoop loop;
    add_watchdog(loop);
    std::vector<int> order;
    std::thread poster{[&] {
        for (int i = 0; i < 1000; ++i) {
            loop.post([&order, i] { order.push_back(i); });
        }
        loop.post([&loop] { loop.quit(0); });
    }};
    CHECK(loop.run() == 0);
    poster.join();
    REQUIRE(order.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
        CHECK(order[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("one-shot and repeating timers") {
    EventLoop loop;
    add_watchdog(loop);
    int once = 0;
    int repeats = 0;
    loop.add_timer(0ms, 0ms, [&] { ++once; });
    EventLoop::SourceId repeating = 0;
    repeating = loop.add_timer(1ms, 1ms, [&] {
        if (++repeats == 5) {
            loop.remove(repeating);
            // Give a removed timer the chance to (wrongly) fire again before quitting.
            loop.add_timer(20ms, 0ms, [&] { loop.quit(0); });
        }
    });
    CHECK(loop.run() == 0);
    CHECK(once == 1);
    CHECK(repeats == 5);
}

TEST_CASE("fd sources: readable callback, removal from inside a callback") {
    EventLoop loop;
    add_watchdog(loop);
    const Pipe first;
    const Pipe second;
    int first_calls = 0;
    int second_calls = 0;
    EventLoop::SourceId second_id = 0;

    loop.add_fd(first.fds[0], POLLIN, [&](short revents) {
        CHECK((revents & POLLIN) != 0);
        first.read_byte();
        ++first_calls;
        loop.remove(second_id);  // both are readable in this iteration: second must not run
        loop.add_timer(20ms, 0ms, [&] { loop.quit(0); });
    });
    second_id = loop.add_fd(second.fds[0], POLLIN, [&](short /*revents*/) {
        second.read_byte();
        ++second_calls;
    });
    first.write_byte();
    second.write_byte();

    CHECK(loop.run() == 0);
    CHECK(first_calls == 1);
    CHECK(second_calls == 0);
}

TEST_CASE("sources added from a callback work, before_poll and notify_always are honoured") {
    EventLoop loop;
    add_watchdog(loop);
    const Pipe idle;  // never written: only notify_always gets its callback called
    int before = 0;
    int notified_idle = 0;
    loop.add_fd(idle.fds[0], POLLIN,
                [&](short revents) {
                    CHECK(revents == 0);
                    ++notified_idle;
                },
                {.before_poll = [&] { ++before; }, .notify_always = true});

    const Pipe late;
    bool late_called = false;
    loop.add_timer(1ms, 0ms, [&] {
        loop.add_fd(late.fds[0], POLLIN, [&](short /*revents*/) {
            late.read_byte();
            late_called = true;
            loop.quit(0);
        });
        late.write_byte();
    });

    CHECK(loop.run() == 0);
    CHECK(late_called);
    CHECK(before >= 2);
    CHECK(notified_idle == before);
}
