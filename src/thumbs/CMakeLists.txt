add_library(rummage_thumbs
    src/cache.cpp
    src/md5.cpp)
add_library(rummage::thumbs ALIAS rummage_thumbs)

target_include_directories(rummage_thumbs PUBLIC include)
target_link_libraries(rummage_thumbs PUBLIC rummage::picture rummage::core PRIVATE rummage::warnings)
