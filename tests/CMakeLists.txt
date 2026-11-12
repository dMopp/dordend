add_executable(nordend-tests
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
    plugin_test.cpp
    remote_test.cpp
    pointer_shapes_test.cpp
    tabs_test.cpp
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
target_link_libraries(nordend-tests PRIVATE nordend::svg nordend::core nordend::theme nordend::fs nordend::views nordend::picture nordend::formats nordend::formats::image nordend::formats::text nordend::text nordend::thumbs nordend::preview nordend::engines nordend::isolate nordend::platform nordend::plugin doctest::doctest nordend::warnings)
# A plugin's own tests, when it is built.
if(TARGET nordend_plugin_remotes)
    target_sources(nordend-tests PRIVATE plugin_remotes_test.cpp)
    target_link_libraries(nordend-tests PRIVATE nordend_plugin_remotes)
endif()

# The helper protocol's private header, for its tests; the image family's private readers (TIFF,
# DNG, lossless JPEG), for theirs.
target_include_directories(nordend-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src
                                                 ${PROJECT_SOURCE_DIR}/src/formats/image/src)

# LibRaw, when installed, as the reference for the DNG reader's values (tests only: never linked
# into Nordend or its helper).
pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw_r)
if(LIBRAW_FOUND)
    target_sources(nordend-tests PRIVATE dng_libraw_test.cpp)
    target_link_libraries(nordend-tests PRIVATE PkgConfig::LIBRAW)
endif()

# exiv2, when installed, as the reference for the metadata readers that replace it (ROADMAP 2e;
# tests only: never linked into Nordend or its helper).
pkg_check_modules(EXIV2_REFERENCE QUIET IMPORTED_TARGET exiv2)
if(EXIV2_REFERENCE_FOUND)
    target_sources(nordend-tests PRIVATE exiv2_reference.cpp exiv2_reference_test.cpp)
    target_link_libraries(nordend-tests PRIVATE PkgConfig::EXIV2_REFERENCE)
    # Nordend's reader against exiv2's, timed (tools, not tests).
    add_executable(nordend-bench-previews bench_previews.cpp exiv2_reference.cpp)
    target_include_directories(nordend-bench-previews PRIVATE ${PROJECT_SOURCE_DIR}/src/formats/image/src)
    target_link_libraries(nordend-bench-previews
        PRIVATE nordend::formats::image PkgConfig::EXIV2_REFERENCE nordend::warnings)
endif()

# A preview helper that misbehaves on purpose (crash, hang, nonsense), and the real one, for
# tests/helpers_test.cpp.
add_executable(nordend-fake-preview fake_preview_helper.cpp
                                    ${PROJECT_SOURCE_DIR}/src/isolate/src/protocol.cpp)
target_include_directories(nordend-fake-preview PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src)
target_link_libraries(nordend-fake-preview PRIVATE nordend::picture nordend::text nordend::warnings)

# The sandbox probe: the preview helper's cage, then one forbidden (or allowed) thing.
add_executable(nordend-sandbox-probe sandbox_probe.cpp
                                     ${PROJECT_SOURCE_DIR}/src/isolate/src/sandbox.cpp)
target_include_directories(nordend-sandbox-probe PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src)
target_link_libraries(nordend-sandbox-probe PRIVATE nordend::warnings)
add_dependencies(nordend-tests nordend-preview nordend-fake-preview nordend-sandbox-probe)
# The shipped example config must parse clean and match the built-in defaults.
target_compile_definitions(nordend-tests
    PRIVATE NORDEND_EXAMPLE_CONFIG="${PROJECT_SOURCE_DIR}/docs/config.example.toml"
            NORDEND_TEST_DATA="${PROJECT_SOURCE_DIR}/tests/data"
            NORDEND_APP_ICON="${PROJECT_SOURCE_DIR}/data/nordend.svg"
            NORDEND_ENGINES="${PROJECT_SOURCE_DIR}/data/engines"
            NORDEND_PREVIEW_HELPER="$<TARGET_FILE:nordend-preview>"
            NORDEND_FAKE_HELPER="$<TARGET_FILE:nordend-fake-preview>"
            NORDEND_SANDBOX_PROBE="$<TARGET_FILE:nordend-sandbox-probe>")
if(NORDEND_SANITIZER)
    target_sources(nordend-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/core/src/lsan_suppressions.cpp)
endif()

add_test(NAME nordend-tests COMMAND nordend-tests)

add_executable(nordend-portal-tests file_picker_portal_test.cpp)
target_link_libraries(nordend-portal-tests PRIVATE nordend::platform PkgConfig::SDBUSCPP nordend::warnings)
find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME file-picker-portal COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:nordend-portal-tests>)
    set_tests_properties(file-picker-portal PROPERTIES TIMEOUT 30)
endif()
