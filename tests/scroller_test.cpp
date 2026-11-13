#include "rummage/ui/scroller.hpp"

#include <chrono>
#include <cmath>

#include <doctest/doctest.h>

using rummage::ui::Scroller;
using namespace std::chrono_literals;

namespace {

double offset(const Scroller& scroller) { return static_cast<double>(scroller.offset()); }

// Runs frames at 60 Hz from t until the scroller stops; returns the time it stopped.
Scroller::Clock::time_point settle(Scroller& scroller, Scroller::Clock::time_point t,
                                   int max_frames = 600) {
    for (int i = 0; i < max_frames && scroller.animating(); ++i) {
        t += 16ms;
        scroller.advance(t);
    }
    return t;
}

}  // namespace

TEST_CASE("scroller: wheel glides to whole steps, notches add up, edges clamp") {
    Scroller s;
    s.set_extent(10000.0F, 500.0F);
    auto t = Scroller::Clock::time_point{} + 1s;
    s.wheel(1.0F, t);
    CHECK(s.animating());
    t += 16ms;
    s.advance(t);
    CHECK(s.offset() > 0.0F);
    CHECK(s.offset() < Scroller::kWheelStep);  // gliding, not jumping
    t = settle(s, t);
    CHECK_FALSE(s.animating());
    CHECK(offset(s) == doctest::Approx(static_cast<double>(Scroller::kWheelStep)));

    // Three quick notches during a glide land three steps further.
    s.wheel(1.0F, t);
    s.wheel(1.0F, t);
    s.wheel(1.0F, t);
    settle(s, t);
    CHECK(offset(s) == doctest::Approx(4.0 * static_cast<double>(Scroller::kWheelStep)));

    s.wheel(-1000.0F, t);
    settle(s, t);
    CHECK(s.offset() == 0.0F);
    s.wheel(1000.0F, t);
    settle(s, t);
    CHECK(offset(s) == doctest::Approx(9500.0));
}

TEST_CASE("scroller: finger follows 1:1, release starts inertia that decays to a stop") {
    Scroller s;
    s.set_extent(100000.0F, 500.0F);
    auto t = Scroller::Clock::time_point{} + 1s;
    // A flick: 20 px every 8 ms (2500 px/s).
    for (int i = 0; i < 6; ++i) {
        t += 8ms;
        s.finger(20.0F, t);
    }
    CHECK(offset(s) == doctest::Approx(120.0));
    CHECK_FALSE(s.animating());  // finger movement is not an animation
    s.release(t);
    CHECK(s.animating());
    const float released_at = s.offset();
    const auto stopped = settle(s, t);
    CHECK_FALSE(s.animating());
    // v0 * tau: 2500 px/s * 0.33 s = 825 px more, minus the tail cut at kMinSpeed.
    CHECK(offset(s) - static_cast<double>(released_at) == doctest::Approx(820.0).epsilon(0.02));
    CHECK(stopped - t < 3s);
}

TEST_CASE("scroller: a slow release does not coast; a pause before release does not either") {
    Scroller s;
    s.set_extent(100000.0F, 500.0F);
    auto t = Scroller::Clock::time_point{} + 1s;
    s.finger(0.05F, t);
    s.release(t + 16ms);
    CHECK_FALSE(s.animating());

    for (int i = 0; i < 6; ++i) {
        t += 8ms;
        s.finger(20.0F, t);
    }
    s.release(t + 300ms);  // the fingers rested on the pad first
    CHECK_FALSE(s.animating());
}

TEST_CASE("scroller: inertia stops at the edge; extent changes clamp; jump_to") {
    Scroller s;
    s.set_extent(1000.0F, 500.0F);
    auto t = Scroller::Clock::time_point{} + 1s;
    for (int i = 0; i < 6; ++i) {
        t += 8ms;
        s.finger(40.0F, t);
    }
    s.release(t);
    settle(s, t);
    CHECK(offset(s) == doctest::Approx(500.0));
    CHECK_FALSE(s.animating());

    s.set_extent(700.0F, 500.0F);  // the folder shrank
    CHECK(offset(s) == doctest::Approx(200.0));
    s.set_extent(300.0F, 500.0F);  // fits entirely
    CHECK(s.offset() == 0.0F);
    CHECK(s.max_offset() == 0.0F);

    s.set_extent(10000.0F, 500.0F);
    s.jump_to(1234.0F);
    CHECK(s.offset() == 1234.0F);
    s.jump_to(1e9F);
    CHECK(s.offset() == 9500.0F);
}

TEST_CASE("scroller: glide_to glides to a clamped offset, destination tells where it heads") {
    Scroller s;
    s.set_extent(1000.0F, 400.0F);
    auto t = Scroller::Clock::time_point{} + 1s;
    s.glide_to(250.0F, t);
    CHECK(s.animating());
    CHECK(static_cast<double>(s.destination()) == doctest::Approx(250.0));
    t += 16ms;
    s.advance(t);
    CHECK(s.offset() > 0.0F);
    CHECK(s.offset() < 250.0F);
    settle(s, t);
    CHECK(offset(s) == doctest::Approx(250.0));
    CHECK(static_cast<double>(s.destination()) == doctest::Approx(250.0));

    s.glide_to(5000.0F, t);  // past the end: clamped
    settle(s, t);
    CHECK(offset(s) == doctest::Approx(600.0));
    s.glide_to(600.0F, t);  // already there: nothing to animate
    CHECK_FALSE(s.animating());
}
