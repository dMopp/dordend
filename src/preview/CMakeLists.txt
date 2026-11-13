add_library(rummage_preview
    src/info.cpp
    src/providers.cpp
    src/summary.cpp)
add_library(rummage::preview ALIAS rummage_preview)

target_include_directories(rummage_preview PUBLIC include)
target_link_libraries(rummage_preview
    PUBLIC rummage::core rummage::fs rummage::picture rummage::text rummage::engines
    PRIVATE rummage::thumbs rummage::warnings)
