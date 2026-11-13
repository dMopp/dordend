cmake_minimum_required(VERSION 3.20)
project(rummage VERSION 0.5.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warning flags — applied to our code only, never to system libraries.
# ---------------------------------------------------------------------------
add_library(rummage_warnings INTERFACE)
add_library(rummage::warnings ALIAS rummage_warnings)
# RUMMAGE_WERROR=OFF for package builds: a newer compiler's new warning must not break them.
option(RUMMAGE_WERROR "Turn warnings into errors" ON)
target_compile_options(rummage_warnings INTERFACE
    -Wall -Wextra -Wpedantic $<$<BOOL:${RUMMAGE_WERROR}>:-Werror>
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
    -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wnull-dereference -Wformat=2 -Wimplicit-fallthrough)

# ---------------------------------------------------------------------------
# Sanitizers for Debug builds in their own directory, e.g.
#   cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRUMMAGE_SANITIZER=address,undefined
# ---------------------------------------------------------------------------
set(RUMMAGE_SANITIZER "" CACHE STRING "Sanitizers to enable (e.g. address,undefined or thread)")
if(RUMMAGE_SANITIZER)
    target_compile_options(rummage_warnings INTERFACE
        -fsanitize=${RUMMAGE_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(rummage_warnings INTERFACE -fsanitize=${RUMMAGE_SANITIZER})
endif()

# Fuzzing (ROADMAP 2c), development only, clang only, in its own directory:
#   CC=clang CXX=clang++ cmake -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#       -DRUMMAGE_FUZZ=ON -DRUMMAGE_WERROR=OFF -DRUMMAGE_BUILD_TESTS=OFF
# Our code is instrumented for libFuzzer, AddressSanitizer and UBSan; tests/fuzz holds the targets.
option(RUMMAGE_FUZZ "Build the libFuzzer targets in tests/fuzz (clang only)" OFF)
if(RUMMAGE_FUZZ)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(FATAL_ERROR "RUMMAGE_FUZZ needs clang (CC=clang CXX=clang++)")
    endif()
    target_compile_options(rummage_warnings INTERFACE
        -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer)
    target_link_options(rummage_warnings INTERFACE -fsanitize=address,undefined)
endif()

option(RUMMAGE_CLANG_TIDY "Run clang-tidy while building the rummage targets" OFF)
if(RUMMAGE_CLANG_TIDY)
    find_program(RUMMAGE_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${RUMMAGE_CLANG_TIDY_EXE}")
endif()

# ---------------------------------------------------------------------------
# System dependencies (pkg-config). Found here so a missing -devel package
# fails at configure time; targets link them as the code starts using them.
# wayland-scanner and wayland-protocols are looked up by src/ui.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl wayland-cursor)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2 REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(HARFBUZZ REQUIRED IMPORTED_TARGET harfbuzz)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(TOMLPLUSPLUS REQUIRED IMPORTED_TARGET tomlplusplus)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET libwebp libwebpdemux)
pkg_check_modules(JXL REQUIRED IMPORTED_TARGET libjxl)
pkg_check_modules(ZLIB REQUIRED IMPORTED_TARGET zlib)
pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)  # udisks2 (ROADMAP 7.3)
pkg_check_modules(TURBOJPEG IMPORTED_TARGET libturbojpeg)  # optional for now: the benchmark

# ---------------------------------------------------------------------------
# Modules
# ---------------------------------------------------------------------------
add_subdirectory(src/core)      # logging, event loop, worker pool, config
add_subdirectory(src/ui)        # Wayland connection, windows, rendering
add_subdirectory(src/theme)     # Noctalia palette, hot reload
add_subdirectory(src/fs)        # directory loading and sorting
add_subdirectory(src/engines)   # engines' manifests: who shows which type (3a)
add_subdirectory(src/text)      # text files: the Text type, the loader (3b)
add_subdirectory(src/picture)   # pictures: Image, turning, scaling, the cache's PNG, the loader
add_subdirectory(src/svg)       # icon-theme SVG rendering on FreeType
add_subdirectory(src/formats)   # the families of files read in the helper (image today)
add_subdirectory(src/thumbs)    # freedesktop thumbnail cache
add_subdirectory(src/preview)   # Quick Look providers: images, text, information
add_subdirectory(src/isolate)   # preview helpers: the parsers in separate processes (2b)
option(RUMMAGE_MEDIA "Build the optional FFmpeg video preview helper" OFF)
if(RUMMAGE_MEDIA)
    # FFmpeg 6.1 introduced the codec-parameter side data used for video rotation.
    pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat>=60.16.100)
    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec>=60.31.102)
    pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil>=58.29.100)
    pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
    add_subdirectory(src/media)
else()
    file(REMOVE "${PROJECT_BINARY_DIR}/engines/media.toml")
endif()
option(RUMMAGE_PDF "Build the optional MuPDF document preview helper" OFF)
if(RUMMAGE_PDF)
    pkg_check_modules(MUPDF REQUIRED IMPORTED_TARGET mupdf)
    add_subdirectory(src/pdf)
else()
    file(REMOVE "${PROJECT_BINARY_DIR}/engines/pdf.toml")
endif()
add_subdirectory(src/platform)  # default applications (freedesktop), launching processes
add_subdirectory(src/views)     # column view and grid

# Renderer test scene (ROADMAP 1.4), shared by the app and rummage-snapshot.
add_library(rummage_test_scene STATIC src/app/test_scene.cpp)
target_include_directories(rummage_test_scene PUBLIC src/app)
target_link_libraries(rummage_test_scene PUBLIC rummage::ui rummage::theme PRIVATE rummage::warnings)

# The commit rummage is built from, for "About" (ROADMAP 7.5d): read when CMake configures, which
# it does again after every commit (it watches .git/logs/HEAD). Empty outside a git checkout.
set(RUMMAGE_COMMIT "")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                    OUTPUT_VARIABLE RUMMAGE_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    if(EXISTS "${PROJECT_SOURCE_DIR}/.git/logs/HEAD")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${PROJECT_SOURCE_DIR}/.git/logs/HEAD")
    endif()
endif()

add_executable(rummage src/app/main.cpp)
target_compile_definitions(rummage PRIVATE RUMMAGE_VERSION="${PROJECT_VERSION}"
                                           RUMMAGE_COMMIT="${RUMMAGE_COMMIT}")
target_link_libraries(rummage
    PRIVATE rummage::core rummage::ui rummage::theme rummage::views rummage::platform
            rummage::isolate rummage_test_scene
            rummage::warnings)

add_executable(rummage-svg-render src/tools/svg_render.cpp)
target_include_directories(rummage-svg-render PRIVATE tests)
target_link_libraries(rummage-svg-render PRIVATE rummage::svg rummage::picture rummage::warnings)

# Development tool: offscreen render of the test scene or a folder list to a PPM file, and the
# list scrolling benchmark. Not installed.
add_executable(rummage-snapshot src/tools/snapshot.cpp)
target_link_libraries(rummage-snapshot
    PRIVATE rummage::core rummage::ui rummage::theme rummage::views rummage::platform
            rummage::isolate rummage_test_scene rummage::warnings)

# Development tool: directory loading benchmark (ROADMAP 3.1). Not installed.
add_executable(rummage-bench-dir src/tools/bench_dir.cpp)
target_link_libraries(rummage-bench-dir PRIVATE rummage::core rummage::fs rummage::warnings)

# Development tool: finding files benchmark (ROADMAP 8b). Not installed.
add_executable(rummage-bench-find src/tools/bench_find.cpp)
target_link_libraries(rummage-bench-find PRIVATE rummage::core rummage::fs rummage::views
                                                 rummage::warnings)

# Development tool: start-up benchmark, runs rummage and times each step of its start
# (ROADMAP 7.6). Not installed.
add_executable(rummage-bench-start src/tools/bench_start.cpp)
target_link_libraries(rummage-bench-start PRIVATE rummage::warnings)

# Development tool: MIME detection check against GLib (ROADMAP 3.2). Not installed.
add_executable(rummage-mime src/tools/mime.cpp)
target_link_libraries(rummage-mime PRIVATE rummage::core rummage::fs rummage::warnings)

# Development tool: live directory model, prints changes (ROADMAP 3.3). Not installed.
add_executable(rummage-watch src/tools/watch.cpp)
target_link_libraries(rummage-watch PRIVATE rummage::core rummage::fs rummage::warnings)

# Development tool: decodes images, prints sizes and timings (ROADMAP 5.1). Not installed.
add_executable(rummage-decode src/tools/decode.cpp)
target_link_libraries(rummage-decode PRIVATE rummage::core rummage::formats::image rummage::warnings)
target_include_directories(rummage-decode PRIVATE ${PROJECT_SOURCE_DIR}/src/formats/image/src)

# Development tool: thumbnails through the freedesktop cache (ROADMAP 5.2). Not installed.
add_executable(rummage-thumb src/tools/thumb.cpp)
target_link_libraries(rummage-thumb PRIVATE rummage::thumbs rummage::formats::image rummage::warnings)

# Development tool: which application opens a file or a type, and how (ROADMAP 6.1). Not
# installed.
add_executable(rummage-open src/tools/open.cpp)
target_link_libraries(rummage-open PRIVATE rummage::core rummage::fs rummage::platform rummage::warnings)

# Development tool: the copy and move engine from the command line, timed (ROADMAP 6.3). Not
# installed.
add_executable(rummage-transfer src/tools/transfer.cpp)
target_link_libraries(rummage-transfer PRIVATE rummage::core rummage::fs rummage::warnings)

# Development tool: JPEG decoding, wuffs against TurboJPEG scaling (ROADMAP 5.2). Not installed;
# built only when libjpeg-turbo-devel is there.
if(TURBOJPEG_FOUND)
    add_executable(rummage-bench-jpeg src/tools/bench_jpeg.cpp)
    target_link_libraries(rummage-bench-jpeg
        PRIVATE rummage::formats::image PkgConfig::TURBOJPEG rummage::warnings)
endif()

# ---------------------------------------------------------------------------
# Tests (doctest, system package). Skipped with a notice when doctest is missing.
# ---------------------------------------------------------------------------
option(RUMMAGE_BUILD_TESTS "Build the unit tests" ON)
if(RUMMAGE_BUILD_TESTS)
    find_package(doctest CONFIG QUIET)
    if(doctest_FOUND)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "doctest not found: unit tests disabled (xbps-install doctest-devel)")
    endif()
endif()

if(RUMMAGE_FUZZ)
    add_subdirectory(tests/fuzz)
endif()

# Known third-party leak suppressions, compiled into every executable of a sanitizer build.
if(RUMMAGE_SANITIZER)
    foreach(executable rummage rummage-preview rummage-snapshot rummage-bench-dir rummage-mime
            rummage-watch rummage-decode rummage-thumb rummage-open rummage-transfer)
        target_sources(${executable} PRIVATE src/core/src/lsan_suppressions.cpp)
    endforeach()
endif()

install(TARGETS rummage RUNTIME DESTINATION bin)
# The preview helper (ROADMAP 2b), found by rummage in ../libexec/rummage/.
install(TARGETS rummage-preview RUNTIME DESTINATION libexec/rummage)
# The desktop entry (app id "rummage"): menus, and the default for folders (inode/directory).
install(FILES data/rummage.desktop DESTINATION share/applications)
# Its icon, named after the app id (the desktop entry's Icon=rummage).
install(FILES data/rummage.svg DESTINATION share/icons/hicolor/scalable/apps)
# The Noctalia template that writes ~/.config/rummage/colors.toml (README, "On Noctalia").
install(FILES data/noctalia/rummage DESTINATION share/rummage/noctalia)

# Full reference, also embedded for per-user first-run setup.
install(FILES docs/config.example.toml DESTINATION share/rummage)

# Optional FileChooser backend; desktops select it in portals.conf.
configure_file(data/org.freedesktop.impl.portal.desktop.rummage.service.in
               org.freedesktop.impl.portal.desktop.rummage.service @ONLY)
install(CODE "configure_file(\"${PROJECT_SOURCE_DIR}/data/org.freedesktop.impl.portal.desktop.rummage.service.in\" \"${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.rummage.service\" @ONLY)")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.rummage.service"
        DESTINATION share/dbus-1/services)
install(FILES data/rummage.portal DESTINATION share/xdg-desktop-portal/portals)
