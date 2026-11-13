#include "rummage/fs/folder_icon.hpp"

#include <sys/stat.h>

#include <doctest/doctest.h>

#include "rummage/core/file.hpp"
#include "rummage/fs/directory.hpp"
#include "temp_dir.hpp"

using rummage::fs::FolderIcons;
using rummage::fs::parse_folder_icon;
using rummage::test::TempDir;
namespace fs = std::filesystem;

TEST_CASE("folder icon settings: preserve unrelated content and escape values") {
    const std::string before =
        "# Dolphin settings\r\n[Desktop Entry]\r\nName=Projects\r\nIcon=old\r\n"
        "Icon=duplicate\r\n[Settings]\r\nHiddenFilesShown=true\r\n";
    const auto after = rummage::fs::update_folder_icon_text(before, "./my icon.svg");
    CHECK(after ==
          "# Dolphin settings\r\n[Desktop Entry]\r\nIcon=./my\\sicon.svg\r\nName=Projects\r\n"
          "[Settings]\r\nHiddenFilesShown=true\r\n");
    CHECK(parse_folder_icon(after, "/work") == "/work/my icon.svg");
    const auto reset = rummage::fs::update_folder_icon_text(after, "");
    CHECK(reset.find("Icon=") == std::string::npos);
    CHECK(reset.find("Name=Projects") != std::string::npos);
    CHECK(reset.find("HiddenFilesShown=true") != std::string::npos);
    CHECK(rummage::fs::update_folder_icon_text("[Settings]\nView=2", "folder-red") ==
          "[Settings]\nView=2\n[Desktop Entry]\nIcon=folder-red\n");
    CHECK(rummage::fs::update_folder_icon_text("", "").empty());
}

TEST_CASE("folder icon settings: atomic save, reset and refusing unsafe metadata") {
    TempDir dir;
    CHECK(rummage::fs::save_folder_icon(dir.path, "folder-music").empty());
    CHECK(rummage::fs::read_folder_icon_setting(dir.path).icon == "folder-music");
    dir.write(".directory", "[Desktop Entry]\nIcon=folder-red\n[Settings]\nView=3\n");
    ::chmod((dir.path / ".directory").c_str(), 0600);
    CHECK(rummage::fs::save_folder_icon(dir.path, "").empty());
    CHECK(rummage::core::read_text_file(dir.path / ".directory").text ==
          "[Desktop Entry]\n[Settings]\nView=3\n");
    CHECK((fs::status(dir.path / ".directory").permissions() & fs::perms::mask) ==
          (fs::perms::owner_read | fs::perms::owner_write));
    fs::remove(dir.path / ".directory");
    dir.write("external", "keep me");
    fs::create_symlink("external", dir.path / ".directory");
    CHECK_FALSE(rummage::fs::save_folder_icon(dir.path, "folder-blue").empty());
    CHECK(fs::is_symlink(dir.path / ".directory"));
    CHECK(rummage::core::read_text_file(dir.path / "external").text == "keep me");
    fs::remove(dir.path / ".directory");
    REQUIRE(::mkfifo((dir.path / ".directory").c_str(), 0600) == 0);
    CHECK_FALSE(rummage::fs::save_folder_icon(dir.path, "folder-blue").empty());
    fs::remove(dir.path / ".directory");
    dir.write(".directory", std::string(65537, 'x'));
    CHECK_FALSE(rummage::fs::save_folder_icon(dir.path, "folder-blue").empty());
    CHECK(fs::file_size(dir.path / ".directory") == 65537);
    CHECK_FALSE(rummage::fs::save_folder_icon(dir.path / "gone", "folder-blue").empty());
    CHECK_FALSE(fs::exists(dir.path / "gone"));
}

TEST_CASE("folder icons: Desktop Entry group, escapes and local paths") {
    CHECK(parse_folder_icon("[Desktop Entry]\nIcon=folder-development\n", "/work") ==
          "folder-development");
    CHECK(parse_folder_icon("Icon=wrong\n[Other]\nIcon=wrong\n", "/work").empty());
    CHECK(parse_folder_icon(" # comment\r\n[Desktop Entry]\r\n Icon = folder-music \r\n"
                            "[Other]\nIcon=wrong\n",
                            "/work") == "folder-music");
    CHECK(parse_folder_icon("[Desktop Entry]\nIcon=./my\\sicon.png\n", "/work") ==
          "/work/my icon.png");
    CHECK(parse_folder_icon("[Desktop Entry]\nIcon=/icons/custom.svg\n", "/work") ==
          "/icons/custom.svg");
    CHECK(parse_folder_icon("[Desktop Entry]\nIcon=folder\\q\n", "/work").empty());
    CHECK(parse_folder_icon("[Desktop Entry]\nIcon=folder\\\n", "/work").empty());
    CHECK(
        parse_folder_icon("[Desktop Entry]\nIcon=https://example.org/icon.svg\n", "/work").empty());
    CHECK(parse_folder_icon("[Desktop Entry]\nIcon=../icon.svg\n", "/work").empty());
    CHECK(parse_folder_icon("[Desktop Entry]\nIcon=\n", "/work").empty());
}

TEST_CASE("folder icons: XDG paths, disabled directories and overrides") {
    TempDir dir;
    const auto home = dir.path / "home";
    fs::create_directories(home / "Musik");
    fs::create_directories(dir.path / "config");
    dir.write("config/user-dirs.dirs",
              "XDG_MUSIC_DIR=\"$HOME/Musik\"\n"
              "XDG_DOWNLOAD_DIR=\"$HOME/\"\n");
    const FolderIcons icons{{.home = home, .config_home = dir.path / "config"}};
    CHECK(icons.for_folder(home) == "user-home");
    CHECK(icons.for_folder(home / "Musik/") == "folder-music");
    CHECK(icons.for_folder(home / "Music").empty());
    CHECK(icons.for_folder(home / "Downloads").empty());
    CHECK(icons.for_folder(dir.path / "elsewhere/Musik").empty());
    dir.write("home/Musik/.directory", "[Desktop Entry]\nIcon=folder-red\n");
    CHECK(icons.for_folder(home / "Musik") == "folder-red");
    dir.write("home/Musik/.directory", "[Other]\nIcon=wrong\n");
    CHECK(icons.for_folder(home / "Musik") == "folder-music");
}

TEST_CASE("folder icons: standard defaults only when user-dirs config is absent") {
    TempDir dir;
    const FolderIcons icons{{.home = dir.path, .config_home = dir.path}};
    CHECK(icons.for_folder(dir.path / "Desktop") == "user-desktop");
    CHECK(icons.for_folder(dir.path / "Documents") == "folder-documents");
    CHECK(icons.for_folder(dir.path / "Downloads") == "folder-download");
    CHECK(icons.for_folder(dir.path / "Music") == "folder-music");
    CHECK(icons.for_folder(dir.path / "Pictures") == "folder-pictures");
    CHECK(icons.for_folder(dir.path / "Videos") == "folder-videos");
    CHECK(icons.for_folder(dir.path / "Public") == "folder-publicshare");
    CHECK(icons.for_folder(dir.path / "Templates") == "folder-templates");
    dir.write("user-dirs.dirs", "");
    const FolderIcons disabled{{.home = dir.path, .config_home = dir.path}};
    CHECK(disabled.for_folder(dir.path / "Music").empty());
}

TEST_CASE("folder icons: malformed, oversized and non-regular metadata is ignored") {
    TempDir dir;
    const FolderIcons icons{{.home = dir.path / "home", .config_home = dir.path}};
    CHECK(icons.for_folder(dir.path).empty());
    dir.write(".directory", std::string(65537, 'x'));
    CHECK(icons.for_folder(dir.path).empty());
    fs::remove(dir.path / ".directory");
    REQUIRE(::mkfifo((dir.path / ".directory").c_str(), 0600) == 0);
    CHECK(icons.for_folder(dir.path).empty());
    fs::remove(dir.path / ".directory");
    fs::create_directory(dir.path / ".directory");
    CHECK(icons.for_folder(dir.path).empty());
}

TEST_CASE("folder icons: directory loads and incremental reads carry icons, including symlinks") {
    TempDir dir;
    fs::create_directory(dir.path / "custom");
    dir.write("custom/.directory", "[Desktop Entry]\nIcon=folder-development\n");
    fs::create_directory_symlink("custom", dir.path / "link");
    dir.write("plain.txt", "text");
    std::vector<rummage::fs::Entry> entries;
    const auto result = rummage::fs::read_directory(dir.path, {}, {}, [&](auto chunk) {
        entries.insert(entries.end(), chunk.begin(), chunk.end());
    });
    REQUIRE_FALSE(result.error);
    REQUIRE(entries.size() == 3);
    for (const auto& entry : entries) {
        CHECK(entry.folder_icon == (entry.is_directory() ? "folder-development" : ""));
    }
    dir.write("custom/.directory", "[Desktop Entry]\nIcon=folder-blue\n");
    const auto updated = rummage::fs::read_entries(dir.path, {"custom", "link"}, {});
    REQUIRE(updated[0]);
    REQUIRE(updated[1]);
    CHECK(updated[0]->folder_icon == "folder-blue");
    CHECK(updated[1]->folder_icon == "folder-blue");
    CHECK(updated[1]->is_link);
}
