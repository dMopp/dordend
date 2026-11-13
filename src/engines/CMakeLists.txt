# rummage's engines (ROADMAP 3a): their manifests and the registry of who shows which type. No
# file parser here: the engines are separate programs.
add_library(rummage_engines
    src/manifest.cpp
    src/registry.cpp)
add_library(rummage::engines ALIAS rummage_engines)

target_include_directories(rummage_engines PUBLIC include)
target_link_libraries(rummage_engines
    PUBLIC rummage::fs
    PRIVATE rummage::core PkgConfig::TOMLPLUSPLUS rummage::warnings)

# The manifests next to rummage in the build tree, where default_directory() looks first.
set(RUMMAGE_ENGINE_MANIFESTS base.toml)
foreach(name IN LISTS RUMMAGE_ENGINE_MANIFESTS)
    configure_file("${PROJECT_SOURCE_DIR}/data/engines/${name}"
                   "${PROJECT_BINARY_DIR}/engines/${name}" COPYONLY)
    install(FILES "${PROJECT_SOURCE_DIR}/data/engines/${name}" DESTINATION share/rummage/engines)
endforeach()
