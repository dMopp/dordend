# What rummage does with a picture, wherever it comes from: the Image type, turning and scaling,
# the thumbnail cache's PNG (written with zlib, read with wuffs), and how a picture is asked for
# (LoadRequest, Loaded, the loader). No parser of outside files: those are in formats/, run by
# the isolated helper.

# wuffs (third_party/wuffs): PNG, JPEG and GIF, memory-safe by construction. Compiled on its own,
# without our warnings.
add_library(rummage_wuffs STATIC src/wuffs_impl.cpp)
target_include_directories(rummage_wuffs SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/wuffs)
target_compile_options(rummage_wuffs PRIVATE -w)
if(RUMMAGE_SANITIZER)
    target_compile_options(rummage_wuffs PRIVATE -fsanitize=${RUMMAGE_SANITIZER})
endif()

add_library(rummage_picture
    src/load.cpp
    src/orientation.cpp
    src/png.cpp
    src/scale.cpp
    src/wuffs.cpp)
add_library(rummage::picture ALIAS rummage_picture)
target_include_directories(rummage_picture PUBLIC include)
target_link_libraries(rummage_picture PRIVATE rummage_wuffs PkgConfig::ZLIB rummage::warnings)
