// Linked only into sanitizer builds (see RUMMAGE_SANITIZER in CMakeLists.txt). Both functions are
// weak hooks the sanitizer runtimes look up; a build without that sanitizer just ignores one.
//
// LeakSanitizer: fontconfig keeps 320 bytes of global state from its first FcFontSort until
// FcFini. The amount does not grow with more calls (checked with a standalone program,
// fontconfig 2.17.1), and calling FcFini would break any other fontconfig user in the process,
// so it is suppressed.
//
// ThreadSanitizer: Mesa's gallium driver runs a GL worker thread (gl0) and synchronises with it
// through code TSan does not instrument, so every GL program reports races inside libgallium
// (seen with Mesa 26.2.5 on radeonsi, also before any rummage thread exists). The same holds
// for libdrm_amdgpu, which that thread and our GL calls on the main thread both go through
// (seen uploading glyphs while Quick Look draws a card). Only reports with a frame in one of
// these libraries are suppressed: races between our own threads still show. It relies on GL
// being called from the main thread only (textures are made while drawing): a worker calling
// GL could race inside the driver unseen.

extern "C" const char*
__lsan_default_suppressions();  // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)
extern "C" const char*
__tsan_default_suppressions();  // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)

extern "C" const char*
__lsan_default_suppressions() {  // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)
    return "leak:libfontconfig.so\n";
}

extern "C" const char*
__tsan_default_suppressions() {  // NOLINT(bugprone-reserved-identifier,readability-identifier-naming)
    return "race:libgallium\n"
           "race:libdrm_amdgpu\n";
}
