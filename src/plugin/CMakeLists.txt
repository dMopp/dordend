# The plugin interface and the set of plugins running (plugin::Plugins). The plugins themselves
# live in plugins/<id>; plugins/CMakeLists.txt builds them and the list of those compiled in.
add_library(nordend_plugin src/plugin.cpp)
add_library(nordend::plugin ALIAS nordend_plugin)

target_include_directories(nordend_plugin PUBLIC include)
target_link_libraries(nordend_plugin
    PUBLIC nordend::core nordend::fs nordend::views
    PRIVATE nordend::warnings)
