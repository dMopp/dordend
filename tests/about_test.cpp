#include "rummage/platform/about.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>

#include "temp_dir.hpp"

using rummage::platform::distribution_name;
using rummage::platform::running;
using rummage::platform::version_from;
using rummage::platform::version_of;
using rummage::test::TempDir;
using namespace std::chrono_literals;

namespace {

// An executable shell script in dir.
std::filesystem::path script(const TempDir& dir, const std::string& name, const std::string& body) {
    const std::filesystem::path file = dir.write(name, "#!/bin/sh\n" + body + "\n");
    std::filesystem::permissions(file, std::filesystem::perms::owner_all);
    return file;
}

}  // namespace

TEST_CASE("about: the distribution's name from os-release") {
    CHECK(distribution_name("NAME=\"Void\"\nID=\"void\"\nPRETTY_NAME=\"Void Linux\"\n") ==
          "Void Linux");
    CHECK(distribution_name("NAME=Arch\nID=arch\n") == "Arch");
    CHECK(distribution_name("PRETTY_NAME='Ubuntu 26.04 \\\"Q\\\"'\n") == "Ubuntu 26.04 \"Q\"");
    CHECK(distribution_name("# nothing\n").empty());
    CHECK(distribution_name("").empty());
}

TEST_CASE("about: the version in a --version line") {
    CHECK(version_from("noctalia v5.4.0") == "5.4.0");
    CHECK(version_from("umbriel 0.1.0 (abc1234)") == "0.1.0 (abc1234)");
    CHECK(version_from("niri 26.04 (unknown commit)\nmore") == "26.04 (unknown commit)");
    CHECK(version_from("  tool V2\n") == "2");
    CHECK(version_from("no version here").empty());
    CHECK(version_from("").empty());
}

TEST_CASE("about: a running program found by name, its own executable") {
    const auto self = running("rummage-tests");
    REQUIRE(self);
    CHECK(std::filesystem::equivalent(self.value_or(""), "/proc/self/exe"));
    CHECK_FALSE(running("no-such-program-x"));
}

TEST_CASE("about: version_of reads the first line, and gives up on a bad or slow program") {
    const TempDir dir;
    CHECK(version_of(script(dir, "good", "echo 'tool v1.2.3'; echo second")) == "tool v1.2.3");
    CHECK(version_of(script(dir, "failing", "echo 'tool v1'; exit 3")).empty());
    CHECK(version_of(dir.path / "missing").empty());
    const auto start = std::chrono::steady_clock::now();
    CHECK(version_of(script(dir, "slow", "sleep 5; echo late"), 100ms).empty());
    CHECK(std::chrono::steady_clock::now() - start < 2s);  // killed, not waited for
}
