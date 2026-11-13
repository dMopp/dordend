# The families of files rummage reads (ROADMAP 2), one module each, and the registry that asks
# them in order. Linked into the isolated helper (rummage-preview), never into rummage.
add_subdirectory(image)  # photos and images
add_subdirectory(text)   # text files (3b)

add_library(rummage_formats src/registry.cpp)
add_library(rummage::formats ALIAS rummage_formats)
target_include_directories(rummage_formats PUBLIC include)
target_link_libraries(rummage_formats
    PUBLIC rummage::picture
    PRIVATE rummage::formats::image rummage::warnings)
