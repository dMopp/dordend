#include "rummage/ui/icon_theme.hpp"

#include <doctest/doctest.h>

#include "rummage/picture/png.hpp"
#include "temp_dir.hpp"

using rummage::test::TempDir;
using rummage::ui::IconTheme;
namespace fs = std::filesystem;

namespace {

void write(const fs::path& root, const std::string& name, std::string_view text) {
    fs::create_directories((root / name).parent_path());
    std::ofstream{root / name} << text;
}

constexpr std::string_view kSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20"><rect width="40" height="20" fill="#ff0000" opacity="0.5"/></svg>)";

}  // namespace

TEST_CASE("icon theme: sizes, inherited themes, split roots and missing names") {
    TempDir dir;
    const fs::path user = dir.path / "user";
    const fs::path system = dir.path / "system";
    write(user, "candy/index.theme", R"([Icon Theme]
Directories=small,scalable
Inherits=parent
[small]
Size=16
Type=Fixed
[scalable]
Size=48
Type=Scalable
MinSize=32
MaxSize=512
)");
    write(user, "candy/small/folder.svg", kSvg);
    write(user, "candy/scalable/folder.svg", kSvg);
    write(system, "candy/scalable/video-x-generic.svg", kSvg);
    write(user, "parent/index.theme",
          "[Icon Theme]\nDirectories=48\nInherits=candy\n[48]\nSize=48\n");
    write(user, "parent/48/text-x-generic.svg", kSvg);
    write(system, "hicolor/index.theme", "[Icon Theme]\nDirectories=48\n[48]\nSize=48\n");
    write(system, "hicolor/48/fallback.svg", kSvg);
    const IconTheme theme{"candy", {user, system}};
    CHECK(theme.find("folder", 16) == user / "candy/small/folder.svg");
    CHECK(theme.find("folder", 384) == user / "candy/scalable/folder.svg");
    CHECK(theme.find("video-x-generic", 192) == system / "candy/scalable/video-x-generic.svg");
    CHECK(theme.find("text-x-generic", 64) == user / "parent/48/text-x-generic.svg");
    CHECK(theme.find("fallback", 64) == system / "hicolor/48/fallback.svg");
    CHECK(theme.find("missing", 64).empty());
    CHECK(theme.find("../folder", 64).empty());
}

TEST_CASE("icon theme: a selected theme wins over a closer inherited size") {
    TempDir dir;
    write(dir.path, "selected/index.theme",
          "[Icon Theme]\nDirectories=16\nInherits=parent\n[16]\nSize=16\nType=Fixed\n");
    write(dir.path, "selected/16/folder.svg", kSvg);
    write(dir.path, "parent/index.theme",
          "[Icon Theme]\nDirectories=128\n[128]\nSize=128\nType=Fixed\n");
    write(dir.path, "parent/128/folder.svg", kSvg);
    const IconTheme theme{"selected", {dir.path}};
    CHECK(theme.find("folder", 128) == dir.path / "selected/16/folder.svg");
}

TEST_CASE("icon theme: scale and user overrides are respected") {
    TempDir dir;
    write(dir.path, "theme/index.theme",
          "[Icon "
          "Theme]\nDirectories=32\nScaledDirectories=32@2\n[32]\nSize=32\nType=Fixed\n[32@2]\nSize="
          "32\nScale=2\nType=Fixed\n");
    write(dir.path, "theme/32/file.svg", kSvg);
    write(dir.path, "theme/32@2/file.svg", kSvg);
    const IconTheme theme{"theme", {dir.path}};
    CHECK(theme.find("file", 64) == dir.path / "theme/32@2/file.svg");
}

TEST_CASE("icon theme: SVG preserves proportions, colour and straight alpha") {
    TempDir dir;
    const auto file = dir.write("icon.svg", kSvg);
    const auto image = IconTheme::render(file, 80);
    REQUIRE_FALSE(image.empty());
    CHECK(image.width == 80);
    CHECK(image.height == 40);
    CHECK(image.rgba[0] == 255);
    CHECK(image.rgba[1] == 0);
    CHECK(image.rgba[2] == 0);
    CHECK(image.rgba[3] == doctest::Approx(128).epsilon(0.01));
    CHECK(IconTheme::render(dir.write("bad.svg", "not svg"), 48).empty());
    CHECK(IconTheme::render(dir.path / "missing.svg", 48).empty());
}

TEST_CASE("icon theme: PNG icons retain their pixels") {
    TempDir dir;
    const rummage::picture::Image original{
        .width = 2, .height = 1, .rgba = {0, 255, 0, 255, 0, 0, 255, 128}};
    const auto png = rummage::picture::encode_png(original);
    const auto file = dir.path / "icon.png";
    {
        std::ofstream output{file, std::ios::binary};
        output.write(reinterpret_cast<const char*>(png.data()),
                     static_cast<std::streamsize>(png.size()));
    }
    const auto image = IconTheme::render(file, 32);
    CHECK(image.rgba == original.rgba);
    CHECK(image.width == 2);
    CHECK(image.height == 1);
}

TEST_CASE("icon theme: absolute custom icons and unavailable overrides") {
    TempDir dir;
    const IconTheme theme{"missing", {dir.path}};
    const auto svg = dir.write("custom.svg", kSvg);
    CHECK(theme.find(svg.string(), 48) == svg);
    CHECK(theme.find((dir.path / "absent.svg").string(), 48).empty());
    CHECK(theme.find(dir.write("other.txt", kSvg).string(), 48).empty());
    fs::create_directory(dir.path / "directory.svg");
    CHECK(theme.find((dir.path / "directory.svg").string(), 48).empty());
    CHECK(theme.find("https://example.org/icon.svg", 48).empty());
}

TEST_CASE("icon theme: folder catalogue deduplicates sizes and excludes broken icons") {
    TempDir dir;
    write(dir.path, "theme/index.theme",
          "[Icon Theme]\nDirectories=16,48\n[16]\nSize=16\n[48]\nSize=48\n");
    write(dir.path, "theme/16/folder-music.svg", kSvg);
    write(dir.path, "theme/48/folder-music.svg", kSvg);
    write(dir.path, "theme/48/folder-red.png", "png");
    write(dir.path, "theme/48/user-home.svg", kSvg);
    write(dir.path, "theme/48/folder-red-symbolic.svg", kSvg);
    write(dir.path, "theme/48/application.svg", kSvg);
    fs::create_symlink("missing", dir.path / "theme/48/folder-broken.svg");
    const IconTheme theme{"theme", {dir.path}};
    CHECK(theme.folder_icons() ==
          std::vector<std::string>{"folder-music", "folder-red", "user-home"});
}

TEST_CASE("icon theme: unsupported SVG filters report fallback without losing the image") {
    TempDir dir;
    const auto file =
        dir.write("unsupported.svg",
                  "<svg width='8' height='8'><defs><filter id='f'><feOffset/></filter></defs><rect "
                  "width='8' height='8' fill='red' filter='url(#f)'/></svg>");
    bool unsupported = false;
    const auto image = IconTheme::render(file, 8, &unsupported);
    REQUIRE_FALSE(image.empty());
    CHECK(unsupported);
    CHECK(image.rgba[0] == 255);
    CHECK(image.rgba[3] == 255);
    CHECK_FALSE(IconTheme::render(dir.write("supported.svg", kSvg), 8, &unsupported).empty());
    CHECK_FALSE(unsupported);
}
