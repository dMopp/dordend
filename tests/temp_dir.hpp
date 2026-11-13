#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <doctest/doctest.h>

namespace rummage::test {

// A temporary directory, removed with everything in it.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "rummage-test-XXXXXX").string();
        REQUIRE(mkdtemp(pattern.data()) != nullptr);
        path = pattern;
    }

    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    // Callers often need only the side effect.
    std::filesystem::path write(  // NOLINT(modernize-use-nodiscard)
        const std::string& name, std::string_view text) const {
        const std::filesystem::path file = path / name;
        std::ofstream{file} << text;
        return file;
    }
};

}  // namespace rummage::test
