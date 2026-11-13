add_library(rummage_svg src/xml.cpp src/css.cpp src/geometry.cpp src/parse.cpp src/paint.cpp src/raster.cpp src/render.cpp src/filter.cpp src/image.cpp)
add_library(rummage::svg ALIAS rummage_svg)
target_include_directories(rummage_svg PUBLIC include)
target_link_libraries(rummage_svg PRIVATE PkgConfig::FREETYPE rummage::picture rummage::warnings)
# The public API returns picture::Image.
target_include_directories(rummage_svg PUBLIC $<TARGET_PROPERTY:rummage_picture,INTERFACE_INCLUDE_DIRECTORIES>)
