add_library(rummage_fs
    src/directory.cpp
    src/directory_model.cpp
    src/directory_watcher.cpp
    src/erase.cpp
    src/folder_icon.cpp
    src/folder_icon_store.cpp
    src/icon.cpp
    src/mime.cpp
    src/operations.cpp
    src/places.cpp
    src/sort.cpp
    src/transfer.cpp
    src/transfer_queue.cpp
    src/trash.cpp
    src/walk.cpp)
add_library(rummage::fs ALIAS rummage_fs)

target_include_directories(rummage_fs PUBLIC include)
target_link_libraries(rummage_fs PUBLIC rummage::core PRIVATE rummage::warnings)
