#include "rummage/ui/scroller.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rummage::ui {
namespace {

// Finger samples older than this do not count for the release velocity.
constexpr std::chrono::milliseconds kVelocityWindow{100};
// The glide is done when this close to its target.
constexpr float kGlideEpsilon = 0.25F;

float seconds(Scroller::Clock::duration duration) {
    return std::chrono::duration<float>(duration).count();
}

}  // namespace

float Scroller::max_offset() const noexcept { return std::max(0.0F, content_ - viewport_); }

float Scroller::clamp(float offset) const noexcept {
    return std::clamp(offset, 0.0F, max_offset());
}

void Scroller::set_extent(float content, float viewport) {
    content_ = content;
    viewport_ = viewport;
    offset_ = clamp(offset_);
    target_ = clamp(target_);
}

void Scroller::wheel(float notches, Clock::time_point now) {
    // Continue from where a running glide is heading, so quick notches add up.
    const float from = mode_ == Mode::glide ? target_ : offset_;
    target_ = clamp(from + (notches * kWheelStep));
    if (target_ != offset_) {
        mode_ = Mode::glide;
        last_ = now;
    }
}

void Scroller::glide_to(float offset, Clock::time_point now) {
    samples_.clear();
    target_ = clamp(offset);
    if (target_ == offset_) {
        mode_ = Mode::idle;
        return;
    }
    if (mode_ != Mode::glide) {
        last_ = now;
    }
    mode_ = Mode::glide;
}

void Scroller::finger(float pixels, Clock::time_point now) {
    mode_ = Mode::finger;
    offset_ = clamp(offset_ + pixels);
    target_ = offset_;
    samples_.push_back({now, pixels});
    while (!samples_.empty() && now - samples_.front().time > kVelocityWindow) {
        samples_.pop_front();
    }
    last_ = now;
}

void Scroller::release(Clock::time_point now) {
    while (!samples_.empty() && now - samples_.front().time > kVelocityWindow) {
        samples_.pop_front();
    }
    // Each sample is the movement since the previous event: the first one happened before the
    // window started, so the distance counts from the second sample on.
    float distance = 0.0F;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
        distance += samples_[i].pixels;
    }
    const float span =
        samples_.size() < 2 ? 0.0F : seconds(samples_.back().time - samples_.front().time);
    samples_.clear();
    velocity_ = span > 0.0F ? distance / span : 0.0F;
    if (std::abs(velocity_) < kMinSpeed) {
        mode_ = Mode::idle;
        return;
    }
    mode_ = Mode::inertia;
    last_ = now;
}

void Scroller::jump_to(float offset) {
    mode_ = Mode::idle;
    samples_.clear();
    offset_ = clamp(offset);
    target_ = offset_;
}

bool Scroller::advance(Clock::time_point now) {
    const float dt = std::max(0.0F, seconds(now - last_));
    last_ = now;
    const float before = offset_;
    switch (mode_) {
        case Mode::idle:
        case Mode::finger:  // moved by finger() itself
            break;
        case Mode::glide: {
            const float step = 1.0F - std::exp(-dt / kGlideSeconds);
            offset_ += (target_ - offset_) * step;
            if (std::abs(target_ - offset_) < kGlideEpsilon) {
                offset_ = target_;
                mode_ = Mode::idle;
            }
            break;
        }
        case Mode::inertia: {
            // Exact integral of v(t) = v0 * exp(-t / tau) over the frame.
            const float decay = std::exp(-dt / kInertiaSeconds);
            offset_ = clamp(offset_ + (velocity_ * kInertiaSeconds * (1.0F - decay)));
            velocity_ *= decay;
            const bool at_edge = offset_ <= 0.0F || offset_ >= max_offset();
            if (std::abs(velocity_) < kMinSpeed || at_edge) {
                mode_ = Mode::idle;
                velocity_ = 0.0F;
            }
            target_ = offset_;
            break;
        }
    }
    return offset_ != before;
}

}  // namespace rummage::ui
