# The text family (ROADMAP 3b): text files, their encoding found from the bytes. Linked into the
# isolated helper (and tests and development tools), never into rummage.
add_library(rummage_formats_text src/languages.cpp src/read.cpp src/syntax.cpp)
add_library(rummage::formats::text ALIAS rummage_formats_text)
target_include_directories(rummage_formats_text PUBLIC include)
target_link_libraries(rummage_formats_text PUBLIC rummage::text PRIVATE rummage::warnings)
