#include "rummage/ui/offscreen.hpp"

#include <EGL/eglext.h>
#include <array>
#include <stdexcept>

namespace rummage::ui {

Offscreen::Offscreen(std::int32_t width, std::int32_t height)
    : display_{eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)} {
    if (display_ == EGL_NO_DISPLAY || eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
        throw std::runtime_error{"surfaceless EGL display unavailable"};
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    // clang-format off
    static constexpr std::array<EGLint, 5> kAttribs{
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE};
    // clang-format on
    context_ = eglCreateContext(display_, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, kAttribs.data());
    if (context_ == EGL_NO_CONTEXT ||
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, context_) != EGL_TRUE) {
        eglTerminate(display_);
        throw std::runtime_error{"cannot create a surfaceless OpenGL ES 3.0 context"};
    }
    glGenRenderbuffers(1, &color_);
    glBindRenderbuffer(GL_RENDERBUFFER, color_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &fbo_);
        glDeleteRenderbuffers(1, &color_);
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display_, context_);
        eglTerminate(display_);
        throw std::runtime_error{"framebuffer incomplete"};
    }
}

Offscreen::~Offscreen() {
    glDeleteFramebuffers(1, &fbo_);
    glDeleteRenderbuffers(1, &color_);
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display_, context_);
    eglTerminate(display_);
}

}  // namespace rummage::ui
