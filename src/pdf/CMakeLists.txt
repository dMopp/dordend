# The only target that links PDFium. The application works without this optional package.
add_executable(rummage-pdf backend.cpp
    ../isolate/src/helper_main.cpp ../isolate/src/protocol.cpp ../isolate/src/sandbox.cpp)
target_include_directories(rummage-pdf PRIVATE ../isolate/src)
target_link_libraries(rummage-pdf PRIVATE rummage::picture rummage::text pdfium
    rummage::warnings)
set_target_properties(rummage-pdf PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
install(TARGETS rummage-pdf RUNTIME DESTINATION libexec/rummage COMPONENT pdf)
configure_file("${PROJECT_SOURCE_DIR}/data/engines/pdf.toml"
    "${PROJECT_BINARY_DIR}/engines/pdf.toml" COPYONLY)
install(FILES "${PROJECT_SOURCE_DIR}/data/engines/pdf.toml"
    DESTINATION share/rummage/engines COMPONENT pdf)
