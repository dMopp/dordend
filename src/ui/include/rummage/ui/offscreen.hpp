#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdint>

namespace rummage::ui {

// OpenGL ES 3.0 without a compositor: a surfaceless EGL context (EGL_MESA_platform_surfaceless)
// made current, drawing into a framebuffer object of width × height pixels. The snapshot tool and
// the tests that draw use it: nothing reaches the user's screen. Throws std::runtime_error when
// the platform has none.
//
// Not copyable or movable.
class Offscreen {
public:
    Offscreen(std::int32_t width, std::int32_t height);
    ~Offscreen();

    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;
    Offscreen(Offscreen&&) = delete;
    Offscreen& operator=(Offscreen&&) = delete;

private:
    EGLDisplay display_;
    EGLContext context_{EGL_NO_CONTEXT};
    GLuint color_{0};
    GLuint fbo_{0};
};

}  // namespace rummage::ui
