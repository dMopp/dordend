add_executable(rummage-tests
    actions_test.cpp
    archives_test.cpp
    main.cpp
    about_test.cpp
    app_icon_test.cpp
    breadcrumb_test.cpp
    clipboard_test.cpp
    column_view_test.cpp
    command_palette_test.cpp
    config_test.cpp
    desktop_palette_test.cpp
    context_menu_test.cpp
    details_view_test.cpp
    devices_test.cpp
    engines_test.cpp
    drag_drop_test.cpp
    directory_model_test.cpp
    directory_test.cpp
    dng_test.cpp
    event_loop_test.cpp
    file_watcher_test.cpp
    format_test.cpp
    folder_icon_test.cpp
    i18n_test.cpp
    icon_test.cpp
    icon_theme_test.cpp
    svg_test.cpp
    svg_features_test.cpp
    svg_layers_test.cpp
    svg_effects_test.cpp
    helpers_test.cpp
    sandbox_test.cpp
    tiff_test.cpp
    image_test.cpp
    load_test.cpp
    memory_test.cpp
    mime_test.cpp
    navigation_test.cpp
    palette_test.cpp
    paste_test.cpp
    places_test.cpp
    remote_test.cpp
    pointer_shapes_test.cpp
    tabs_test.cpp
    panes_test.cpp
    text_test.cpp
    text_selection_test.cpp
    platform_test.cpp
    preview_test.cpp
    quick_look_test.cpp
    rename_test.cpp
    renderer_runs_test.cpp
    scroller_test.cpp
    shortcuts_test.cpp
    sort_test.cpp
    syntax_test.cpp
    syntax_colors_test.cpp
    state_test.cpp
    thumbs_test.cpp
    title_bar_test.cpp
    erase_test.cpp
    erase_view_test.cpp
    file_finder_test.cpp
    file_picker_test.cpp
    transfer_test.cpp
    trash_test.cpp
    trash_view_test.cpp
    undo_test.cpp
    worker_pool_test.cpp
    xcursor_test.cpp)
target_link_libraries(rummage-tests PRIVATE rummage::svg rummage::core rummage::theme rummage::fs rummage::views rummage::picture rummage::formats rummage::formats::image rummage::formats::text rummage::text rummage::thumbs rummage::preview rummage::engines rummage::isolate rummage::platform doctest::doctest rummage::warnings)

# The helper protocol's private header, for its tests; the image family's private readers (TIFF,
# DNG, lossless JPEG), for theirs.
target_include_directories(rummage-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src
                                                 ${PROJECT_SOURCE_DIR}/src/formats/image/src)

# LibRaw, when installed, as the reference for the DNG reader's values (tests only: never linked
# into rummage or its helper).
pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw_r)
if(LIBRAW_FOUND)
    target_sources(rummage-tests PRIVATE dng_libraw_test.cpp)
    target_link_libraries(rummage-tests PRIVATE PkgConfig::LIBRAW)
endif()

# exiv2, when installed, as the reference for the metadata readers that replace it (ROADMAP 2e;
# tests only: never linked into rummage or its helper).
pkg_check_modules(EXIV2_REFERENCE QUIET IMPORTED_TARGET exiv2)
if(EXIV2_REFERENCE_FOUND)
    target_sources(rummage-tests PRIVATE exiv2_reference.cpp exiv2_reference_test.cpp)
    target_link_libraries(rummage-tests PRIVATE PkgConfig::EXIV2_REFERENCE)
    # rummage's reader against exiv2's, timed (tools, not tests).
    add_executable(rummage-bench-previews bench_previews.cpp exiv2_reference.cpp)
    target_include_directories(rummage-bench-previews PRIVATE ${PROJECT_SOURCE_DIR}/src/formats/image/src)
    target_link_libraries(rummage-bench-previews
        PRIVATE rummage::formats::image PkgConfig::EXIV2_REFERENCE rummage::warnings)
endif()

# A preview helper that misbehaves on purpose (crash, hang, nonsense), and the real one, for
# tests/helpers_test.cpp.
add_executable(rummage-fake-preview fake_preview_helper.cpp
                                    ${PROJECT_SOURCE_DIR}/src/isolate/src/protocol.cpp)
target_include_directories(rummage-fake-preview PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src)
target_link_libraries(rummage-fake-preview PRIVATE rummage::picture rummage::text rummage::warnings)

# The sandbox probe: the preview helper's cage, then one forbidden (or allowed) thing.
add_executable(rummage-sandbox-probe sandbox_probe.cpp
                                     ${PROJECT_SOURCE_DIR}/src/isolate/src/sandbox.cpp)
target_include_directories(rummage-sandbox-probe PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src)
target_link_libraries(rummage-sandbox-probe PRIVATE rummage::warnings)
add_dependencies(rummage-tests rummage-preview rummage-fake-preview rummage-sandbox-probe)
# The shipped example config must parse clean and match the built-in defaults.
target_compile_definitions(rummage-tests
    PRIVATE RUMMAGE_EXAMPLE_CONFIG="${PROJECT_SOURCE_DIR}/docs/config.example.toml"
            RUMMAGE_TEST_DATA="${PROJECT_SOURCE_DIR}/tests/data"
            RUMMAGE_APP_ICON="${PROJECT_SOURCE_DIR}/data/rummage.svg"
            RUMMAGE_ENGINES="${PROJECT_SOURCE_DIR}/data/engines"
            RUMMAGE_PREVIEW_HELPER="$<TARGET_FILE:rummage-preview>"
            RUMMAGE_FAKE_HELPER="$<TARGET_FILE:rummage-fake-preview>"
            RUMMAGE_SANDBOX_PROBE="$<TARGET_FILE:rummage-sandbox-probe>")
if(RUMMAGE_SANITIZER)
    target_sources(rummage-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/core/src/lsan_suppressions.cpp)
endif()

# Allocator thresholds depend on earlier allocations: give memory accounting its own process.
add_test(NAME rummage-tests COMMAND rummage-tests "--source-file-exclude=*memory_test.cpp")
add_test(NAME memory-tests COMMAND rummage-tests "--source-file=*memory_test.cpp")

add_executable(rummage-portal-tests file_picker_portal_test.cpp)
target_link_libraries(rummage-portal-tests PRIVATE rummage::platform PkgConfig::SDBUSCPP rummage::warnings)
add_executable(rummage-notification-tests notifications_test.cpp)
target_include_directories(rummage-notification-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/platform/src)
target_link_libraries(rummage-notification-tests PRIVATE rummage::platform PkgConfig::SDBUSCPP rummage::warnings)
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME file-picker-portal COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:rummage-portal-tests>)
    set_tests_properties(file-picker-portal PROPERTIES TIMEOUT 30)
    add_test(NAME action-notifications COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:rummage-notification-tests>)
    set_tests_properties(action-notifications PROPERTIES TIMEOUT 15)
endif()

if(RUMMAGE_MEDIA)
    add_executable(rummage-media-tests media_test.cpp)
    target_link_libraries(rummage-media-tests PRIVATE doctest::doctest rummage::isolate
        rummage::thumbs rummage::preview rummage::warnings)
    target_compile_definitions(rummage-media-tests PRIVATE
        RUMMAGE_MEDIA_HELPER="$<TARGET_FILE:rummage-media>"
        RUMMAGE_BASE_HELPER="$<TARGET_FILE:rummage-preview>"
        RUMMAGE_FAKE_HELPER="$<TARGET_FILE:rummage-fake-preview>"
        RUMMAGE_MEDIA_DATA="${PROJECT_SOURCE_DIR}/tests/data/media"
        RUMMAGE_IMAGE_DATA="${PROJECT_SOURCE_DIR}/tests/data/image"
        RUMMAGE_ENGINE_DATA="${PROJECT_SOURCE_DIR}/data/engines")
    add_dependencies(rummage-media-tests rummage-media rummage-preview rummage-fake-preview)
    add_test(NAME media-tests COMMAND rummage-media-tests)
    set_tests_properties(media-tests PROPERTIES TIMEOUT 60)
endif()

if(RUMMAGE_PDF)
    add_executable(rummage-pdf-tests pdf_test.cpp)
    target_link_libraries(rummage-pdf-tests PRIVATE doctest::doctest rummage::isolate
        rummage::thumbs rummage::preview rummage::warnings)
    target_compile_definitions(rummage-pdf-tests PRIVATE
        RUMMAGE_PDF_HELPER="$<TARGET_FILE:rummage-pdf>"
        RUMMAGE_BASE_HELPER="$<TARGET_FILE:rummage-preview>"
        RUMMAGE_PDF_DATA="${PROJECT_SOURCE_DIR}/tests/data/pdf"
        RUMMAGE_ENGINE_DATA="${PROJECT_SOURCE_DIR}/data/engines")
    add_dependencies(rummage-pdf-tests rummage-pdf rummage-preview)
    add_test(NAME pdf-tests COMMAND rummage-pdf-tests)
    set_tests_properties(pdf-tests PROPERTIES TIMEOUT 60)
endif()
