add_library(rummage_platform
    src/action_handoff.cpp
    src/archives.cpp
    src/about.cpp
    src/applications.cpp
    src/bus_loop.cpp
    src/desktop_entry.cpp
    src/exec.cpp
    src/file_picker_portal.cpp
    src/gvfs.cpp
    src/launcher.cpp
    src/notifications.cpp
    src/opener.cpp
    src/portal.cpp
    src/udisks.cpp
    src/volumes.cpp)
add_library(rummage::platform ALIAS rummage_platform)

target_include_directories(rummage_platform PUBLIC include)
# thumbs: file_uri(), the file:// URIs %u and %U take, escaped as GLib does.
target_link_libraries(rummage_platform
    PUBLIC rummage::core rummage::fs
    PRIVATE rummage::thumbs rummage::warnings PkgConfig::SDBUSCPP)
