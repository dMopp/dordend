# libFuzzer targets (ROADMAP 2c, 2d), built with RUMMAGE_FUZZ=ON (see the top CMakeLists.txt).
#   build-fuzz/tests/fuzz/rummage-fuzz-tiff CORPUS tests/data/image/tiff -max_total_time=3600
add_executable(rummage-fuzz-tiff fuzz_tiff.cpp)
target_include_directories(rummage-fuzz-tiff
    PRIVATE ${PROJECT_SOURCE_DIR}/src/formats/image/src)
target_link_libraries(rummage-fuzz-tiff PRIVATE rummage::formats::image rummage::warnings)
target_link_options(rummage-fuzz-tiff PRIVATE -fsanitize=fuzzer)

# Camera files' previews (ROADMAP 2e): seeds are the first bytes of raw files, kept locally.
add_executable(rummage-fuzz-camera fuzz_camera.cpp)
target_link_libraries(rummage-fuzz-camera PRIVATE rummage::formats::image rummage::warnings)
target_link_options(rummage-fuzz-camera PRIVATE -fsanitize=fuzzer)

add_executable(rummage-fuzz-picture fuzz_picture.cpp)
target_link_libraries(rummage-fuzz-picture PRIVATE rummage::formats rummage::warnings)
target_link_options(rummage-fuzz-picture PRIVATE -fsanitize=fuzzer)

# DNG (ROADMAP 2d): seeds from tests/fuzz/make_dng_seeds.py and tests/data/image/dng.
foreach(target dng ljpeg)
    add_executable(rummage-fuzz-${target} fuzz_${target}.cpp)
    target_include_directories(rummage-fuzz-${target}
        PRIVATE ${PROJECT_SOURCE_DIR}/src/formats/image/src)
    target_link_libraries(rummage-fuzz-${target} PRIVATE rummage::formats::image rummage::warnings)
    target_link_options(rummage-fuzz-${target} PRIVATE -fsanitize=fuzzer)
endforeach()

# Text (ROADMAP 3b): seeds are a few text files in different encodings.
add_executable(rummage-fuzz-text fuzz_text.cpp)
target_link_libraries(rummage-fuzz-text PRIVATE rummage::formats::text rummage::warnings)
target_link_options(rummage-fuzz-text PRIVATE -fsanitize=fuzzer)

add_executable(rummage-fuzz-svg fuzz_svg.cpp)
target_link_libraries(rummage-fuzz-svg PRIVATE rummage::svg rummage::warnings)
target_link_options(rummage-fuzz-svg PRIVATE -fsanitize=fuzzer)
