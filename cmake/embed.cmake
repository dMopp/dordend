# Writes a file's bytes into a C++ source as an array, so the program carries the file in it.
#   cmake -DINPUT=file -DOUTPUT=file.cpp -DSYMBOL=name -P cmake/embed.cmake
# The source defines rummage::embedded::SYMBOL (const unsigned char[]) and SYMBOL_size.
file(READ "${INPUT}" hex HEX)
string(LENGTH "${hex}" length)
math(EXPR size "${length} / 2")
# Sixteen bytes per line.
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " bytes "${bytes}")
get_filename_component(name "${INPUT}" NAME)
file(WRITE "${OUTPUT}" "// Generated from ${name} by cmake/embed.cmake: do not edit.
#include <cstddef>

namespace rummage::embedded {

extern const unsigned char ${SYMBOL}[];
extern const std::size_t ${SYMBOL}_size;

const unsigned char ${SYMBOL}[] = {
    ${bytes}
};
const std::size_t ${SYMBOL}_size = ${size};

}  // namespace rummage::embedded
")
