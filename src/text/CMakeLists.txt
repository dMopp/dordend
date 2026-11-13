# What rummage does with a text file (ROADMAP 3b): the Text type and how one is asked for. No
# reader of files: those are in formats/text, run by the base engine.
add_library(rummage_text src/text.cpp)
add_library(rummage::text ALIAS rummage_text)
target_include_directories(rummage_text PUBLIC include)
target_link_libraries(rummage_text PRIVATE rummage::warnings)
