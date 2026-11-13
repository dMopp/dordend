#include "rummage/ui/egl_context.hpp"

#include <EGL/eglext.h>
#include <array>
#include <format>
#include <stdexcept>

#include "rummage/core/log.hpp"

namespace rummage::ui {
namespace {

[[noreturn]] void throw_egl(const char* what) {
    throw std::runtime_error{std::format("{} failed (EGL error 0x{:04x})", what, eglGetError())};
}

}  // namespace

EglContext::EglContext(const Display& display)
    : display_{eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display.native(), nullptr)} {
    if (display_ == EGL_NO_DISPLAY) {
        throw_egl("eglGetPlatformDisplay");
    }
    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display_, &major, &minor) != EGL_TRUE) {
        throw_egl("eglInitialize");
    }
    log::debug("EGL {}.{} ({})", major, minor, eglQueryString(display_, EGL_VENDOR));

    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        eglTerminate(display_);
        throw_egl("eglBindAPI");
    }

    // Alpha carries background transparency to the compositor; foreground stays opaque.
    // clang-format off
    static constexpr std::array<EGLint, 13> kConfigAttribs{
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE};
    // clang-format on
    EGLint count = 0;
    if (eglChooseConfig(display_, kConfigAttribs.data(), &config_, 1, &count) != EGL_TRUE ||
        count == 0) {
        eglTerminate(display_);
        throw_egl("eglChooseConfig (RGBA8888, OpenGL ES 3)");
    }

    // clang-format off
    static constexpr std::array<EGLint, 5> kContextAttribs{
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE};
    // clang-format on
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, kContextAttribs.data());
    if (context_ == EGL_NO_CONTEXT) {
        eglTerminate(display_);
        throw_egl("eglCreateContext (OpenGL ES 3.0)");
    }
}

EglContext::~EglContext() {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display_, context_);
    eglTerminate(display_);
    eglReleaseThread();
}

}  // namespace rummage::ui
