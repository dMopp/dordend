#pragma once

#include <EGL/egl.h>

#include "rummage/ui/display.hpp"

namespace rummage::ui {

// EGL display, config and OpenGL ES 3.0 context shared by every window of the app.
// Throws std::runtime_error if EGL or a suitable config/context is unavailable.
// Must not outlive the Display. Not copyable or movable.
class EglContext {
public:
    explicit EglContext(const Display& display);
    ~EglContext();

    EglContext(const EglContext&) = delete;
    EglContext& operator=(const EglContext&) = delete;
    EglContext(EglContext&&) = delete;
    EglContext& operator=(EglContext&&) = delete;

    [[nodiscard]] EGLDisplay display() const noexcept { return display_; }

    [[nodiscard]] EGLConfig config() const noexcept { return config_; }

    [[nodiscard]] EGLContext context() const noexcept { return context_; }

private:
    EGLDisplay display_;
    EGLConfig config_{nullptr};
    EGLContext context_{nullptr};
};

}  // namespace rummage::ui
