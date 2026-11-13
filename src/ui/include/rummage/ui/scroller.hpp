#pragma once

#include <chrono>
#include <cstdint>
#include <deque>

namespace rummage::ui {

// Scrolling physics for one scrollable area along one axis, in logical pixels. No drawing and no
// clock of its own: input and frames come with their time, so it can be tested and replayed.
//
// - Wheel: each notch moves a target; the offset glides there (exponential approach), so fast
//   wheel turns accumulate and the motion stays smooth.
// - Touchpad: the content follows the fingers 1:1; when they lift, the release velocity carries
//   on and decays exponentially (inertia), as on macOS and GNOME.
// - The offset stays within [0, content - viewport].
//
// Call advance(now) once per frame while animating() is true, before drawing.
class Scroller {
public:
    using Clock = std::chrono::steady_clock;

    // Logical pixels per wheel notch.
    static constexpr float kWheelStep = 48.0F;
    // Time constant of the wheel glide: about 95 % of the way in three of these.
    static constexpr float kGlideSeconds = 0.05F;
    // Time constant of the inertia decay.
    static constexpr float kInertiaSeconds = 0.33F;
    // Inertia stops below this speed (logical pixels per second).
    static constexpr float kMinSpeed = 15.0F;

    // Content and viewport heights; the offset is clamped to the new range.
    void set_extent(float content, float viewport);

    // A wheel turn of `notches` (positive = down; fractional on high-resolution wheels).
    void wheel(float notches, Clock::time_point now);

    // Touchpad movement of `pixels` (positive = content moves up, as scroll_y from the seat).
    void finger(float pixels, Clock::time_point now);

    // The fingers left the touchpad: start inertia from the recent movement.
    void release(Clock::time_point now);

    // Glides to an offset, as a wheel turn would (keeping a cursor in view, revealing a column).
    void glide_to(float offset, Clock::time_point now);

    // Jumps without animation (keyboard Home/End, opening a folder).
    void jump_to(float offset);

    // Moves the animation to `now`. Returns true if the offset changed.
    bool advance(Clock::time_point now);

    // A glide or inertia is running: keep drawing frames. (Finger movement redraws through its
    // own input events.)
    [[nodiscard]] bool animating() const noexcept {
        return mode_ == Mode::glide || mode_ == Mode::inertia;
    }

    [[nodiscard]] float offset() const noexcept { return offset_; }

    // Where the offset is heading: the glide's destination while gliding, else the offset.
    [[nodiscard]] float destination() const noexcept {
        return mode_ == Mode::glide ? target_ : offset_;
    }

    [[nodiscard]] float max_offset() const noexcept;

private:
    enum class Mode : std::uint8_t { idle, glide, finger, inertia };

    struct Sample {
        Clock::time_point time;
        float pixels{0.0F};
    };

    [[nodiscard]] float clamp(float offset) const noexcept;

    Mode mode_{Mode::idle};
    float offset_{0.0F};
    float target_{0.0F};    // glide destination
    float velocity_{0.0F};  // inertia, logical pixels per second
    float content_{0.0F};
    float viewport_{0.0F};
    Clock::time_point last_;
    std::deque<Sample> samples_;  // recent finger movement, for the release velocity
};

}  // namespace rummage::ui
