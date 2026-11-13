#include "rummage/core/file_picker.hpp"

#include <chrono>
#include <fstream>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include <doctest/doctest.h>

#include "rummage/views/file_picker.hpp"
#include "temp_dir.hpp"

namespace {
namespace core = rummage::core;
namespace fs = std::filesystem;
namespace views = rummage::views;
using rummage::test::TempDir;
using Mode = core::PickerRequest::Mode;
using namespace std::chrono_literals;

struct MemoryFile {
    int fd{memfd_create("picker-test", MFD_CLOEXEC)};

    ~MemoryFile() { close(fd); }
};

struct Fixture {
    TempDir temp;
    core::EventLoop loop;
    core::WorkerPool pool{loop, 2};
    rummage::fs::DirectoryWatcher watcher{loop};
    std::shared_ptr<const rummage::fs::MimeDatabase> mime = rummage::fs::MimeDatabase::load();
    views::Browser browser{loop, pool, watcher, mime, [] {}, [](auto) {}};
    std::optional<core::PickerResult> result;

    Fixture() {
        fs::create_directories(temp.path / "folder");
        std::ofstream{temp.path / "one.txt"} << "one";
        std::ofstream{temp.path / "two.png"} << "two";
        browser.set_home_trash(temp.path / "Trash");
        browser.open(temp.path);
    }

    bool settle(std::function<bool()> ready = {}) {
        bool done = false;
        const auto poll = loop.add_timer(1ms, 1ms, [&] {
            if (!browser.loading() && (!ready || ready())) {
                done = true;
                loop.quit();
            }
        });
        const auto stop = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
        loop.run();
        loop.remove(poll);
        loop.remove(stop);
        return done;
    }

    auto picker(core::PickerRequest request) {
        return std::make_unique<views::FilePicker>(
            browser, std::move(request), mime, [] {},
            [this](core::PickerResult chosen) { result = std::move(chosen); });
    }
};
}  // namespace

TEST_CASE("picker: request and result round trip preserves arbitrary filename bytes") {
    MemoryFile file;
    REQUIRE(file.fd >= 0);
    core::PickerRequest request;
    request.mode = Mode::save_files;
    request.multiple = true;
    request.directory = true;
    request.title = "Save photos";
    request.accept_label = "Choose";
    request.folder = "/tmp/directory with spaces";
    request.name = "photo\nname";
    request.parent_window = "wayland:token";
    request.modal = false;
    request.files = {"a\nb", std::string{"raw-"} + static_cast<char>(0xff)};
    request.filters = {{"Text", {{0, "*.txt"}, {1, "text/plain"}}}, {"All", {{0, "*"}}}};
    request.filter = 1;
    request.choices = {{"encoding", "Encoding", {{"utf8", "UTF-8"}}, "utf8"},
                       {"readonly", "Read only", {}, "true"}};
    core::write_picker_request(file.fd, request);
    CHECK(core::read_picker_request(file.fd) == request);
    core::PickerResult result{0, {"/tmp/a\nb", "/tmp/quote'file"}, 1, {"utf8", "true"}};
    core::write_picker_result(file.fd, result);
    CHECK(core::read_picker_result(file.fd) == result);
    CHECK_THROWS(core::read_picker_request(file.fd));
    REQUIRE(ftruncate(file.fd, 3) == 0);
    CHECK_THROWS(core::read_picker_result(file.fd));
    request.name = std::string{"a\0b", 3};
    CHECK_THROWS(core::write_picker_request(file.fd, request));
}

TEST_CASE("picker: invalid protocol indexes and lengths are rejected") {
    MemoryFile file;
    core::PickerRequest request;
    request.filter = 99;
    core::write_picker_request(file.fd, request);
    CHECK_THROWS(core::read_picker_request(file.fd));
    CHECK(ftruncate(file.fd, 5 * 1024 * 1024) == 0);
    CHECK_THROWS(core::read_picker_request(file.fd));
    core::PickerResult result{8, {}, 0, {}};
    core::write_picker_result(file.fd, result);
    CHECK_THROWS(core::read_picker_result(file.fd));
}

TEST_CASE("picker: open files directories and multiple selection are validated") {
    TempDir temp;
    fs::create_directory(temp.path / "folder");
    std::ofstream{temp.path / "a\nb"} << "data";
    std::ofstream{temp.path / "second"} << "data";
    core::PickerRequest request;
    CHECK_FALSE(views::picker_selection(request, temp.path, {}, "").error.empty());
    CHECK_FALSE(views::picker_selection(request, temp.path, {"gone"}, "").error.empty());
    CHECK_FALSE(views::picker_selection(request, temp.path, {"folder"}, "").error.empty());
    CHECK(views::picker_selection(request, temp.path, {"a\nb"}, "").paths ==
          std::vector<std::string>{(temp.path / "a\nb").string()});
    CHECK_FALSE(views::picker_selection(request, temp.path, {"a\nb", "second"}, "").error.empty());
    request.multiple = true;
    CHECK(views::picker_selection(request, temp.path, {"a\nb", "second"}, "").paths.size() == 2);
    request.directory = true;
    CHECK(views::picker_selection(request, temp.path, {}, "").paths ==
          std::vector<std::string>{temp.path.string()});
    CHECK(views::picker_selection(request, temp.path, {"folder"}, "").error.empty());
    CHECK_FALSE(views::picker_selection(request, temp.path, {"second"}, "").error.empty());
}

TEST_CASE("picker: saving never writes and requires confirmation for existing files") {
    TempDir temp;
    core::PickerRequest request;
    request.mode = Mode::save;
    std::ofstream{temp.path / "existing"} << "original";
    fs::create_directory(temp.path / "folder");
    fs::create_symlink(temp.path / "missing", temp.path / "broken");
    const auto fresh = views::picker_selection(request, temp.path, {}, "new.txt");
    CHECK(fresh.error.empty());
    CHECK_FALSE(fresh.overwrite);
    CHECK_FALSE(fs::exists(temp.path / "new.txt"));
    const auto existing = views::picker_selection(request, temp.path, {}, "existing");
    CHECK(existing.error.empty());
    CHECK(existing.overwrite);
    CHECK(fs::file_size(temp.path / "existing") == 8);
    for (const auto* name : {"", ".", "..", "../escape", "folder", "broken"}) {
        CHECK_FALSE(views::picker_selection(request, temp.path, {}, name).error.empty());
    }
    request.mode = Mode::save_files;
    request.files = {"new.txt", "existing"};
    const auto batch = views::picker_selection(request, temp.path, {}, "");
    CHECK(batch.paths.size() == 2);
    CHECK(batch.overwrite);
    CHECK(views::picker_selection(request, temp.path, {"folder"}, "").paths.front() ==
          (temp.path / "folder/new.txt").string());
    request.files = {"../escape"};
    CHECK_FALSE(views::picker_selection(request, temp.path, {}, "").error.empty());
}

TEST_CASE("picker: filters preserve directories and return the chosen filter and choices") {
    Fixture f;
    core::PickerRequest request;
    request.filters = {{"Text", {{0, "*.txt"}}}, {"Images", {{1, "image/*"}}}};
    request.choices = {{"encoding", "Encoding", {{"utf8", "UTF-8"}}, ""}};
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    auto folder = f.browser.picker_folder();
    REQUIRE(folder);
    CHECK(folder->entries().size() == 2);
    CHECK(folder->entries().back().name == "one.txt");
    picker->set_filter(1);
    CHECK(folder->entries().size() == 2);
    CHECK(folder->entries().back().name == "two.png");
    folder->select_name("two.png");
    picker->accept();
    REQUIRE(f.result);
    CHECK(f.result->response == 0);
    CHECK(f.result->filter == 1);
    CHECK(f.result->choices == std::vector<std::string>{"utf8"});
    CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / "two.png").string()});
}

TEST_CASE("picker: filtering survives view changes and opening new folders") {
    Fixture f;
    std::ofstream{f.temp.path / "folder/inside.txt"} << "data";
    std::ofstream{f.temp.path / "folder/other.png"} << "data";
    core::PickerRequest request;
    request.filters = {{"Text", {{0, "*.txt"}}}};
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    for (const auto mode : {views::Browser::Mode::grid, views::Browser::Mode::details,
                            views::Browser::Mode::columns}) {
        f.browser.set_mode(mode);
        f.browser.open(f.temp.path / "folder");
        REQUIRE(f.settle());
        const auto folder = f.browser.picker_folder();
        REQUIRE(folder);
        REQUIRE(folder->entries().size() == 1);
        CHECK(folder->entries()[0].name == "inside.txt");
    }
}

TEST_CASE(
    "picker: Enter selects a file, Escape cancels and directory mode chooses current folder") {
    SUBCASE("open") {
        Fixture f;
        auto picker = f.picker({});
        REQUIRE(f.settle());
        f.browser.picker_folder()->select_name("one.txt");
        picker->key({.keysym = XKB_KEY_Return, .text = {}});
        REQUIRE(f.result);
        CHECK(f.result->response == 0);
    }
    SUBCASE("cancel") {
        Fixture f;
        auto picker = f.picker({});
        REQUIRE(f.settle());
        picker->key({.keysym = XKB_KEY_Escape, .text = {}});
        REQUIRE(f.result);
        CHECK(f.result->response == 1);
        picker->accept();
        CHECK(f.result->response == 1);
    }
    SUBCASE("folder") {
        Fixture f;
        core::PickerRequest request;
        request.directory = true;
        auto picker = f.picker(request);
        REQUIRE(f.settle());
        CHECK(f.browser.picker_folder()->entries().size() == 1);
        picker->key({.keysym = XKB_KEY_Return, .text = {}, .modifiers = {.ctrl = true}});
        REQUIRE(f.result);
        CHECK(f.result->paths == std::vector<std::string>{f.temp.path.string()});
    }
}

TEST_CASE("picker: overwrite confirmation can be cancelled without closing picker") {
    Fixture f;
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "one.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    picker->accept();
    CHECK_FALSE(f.result);
    picker->key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK_FALSE(f.result);
    picker->accept();
    CHECK_FALSE(f.result);
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(f.result);
    CHECK(f.result->response == 0);
    CHECK(fs::file_size(f.temp.path / "one.txt") == 3);
}

TEST_CASE("picker: choosing a filename in save mode replaces the initial suggestion") {
    Fixture f;
    core::PickerRequest request;
    request.mode = Mode::save;
    request.name = "Untitled.txt";
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    // Shift+Tab returns from the name field to the browser.
    picker->key({.keysym = XKB_KEY_Tab, .text = {}, .modifiers = {.shift = true}});
    f.browser.picker_folder()->select_name("one.txt");
    picker->key({.keysym = XKB_KEY_Control_L, .text = {}});
    picker->key({.keysym = XKB_KEY_Return, .text = {}, .modifiers = {.ctrl = true}});
    CHECK_FALSE(f.result);  // the existing filename needs confirmation
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    REQUIRE(f.result);
    CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / "one.txt").string()});
    CHECK_FALSE(fs::exists(f.temp.path / "Untitled.txt"));
}

TEST_CASE("picker: missing typed paths have useful errors and folder labels") {
    Fixture f;
    core::PickerRequest request;
    request.directory = true;
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    CHECK(std::string{picker->name_label()} == "Folder");
    picker->set_name("not-created");
    picker->accept();
    CHECK(picker->error().find("does not exist") != std::string::npos);
    CHECK(picker->error().find("New Folder") != std::string::npos);
    CHECK_FALSE(f.result);
    CHECK_FALSE(fs::exists(f.temp.path / "not-created"));
    picker->set_name("one.txt");
    CHECK(picker->error().empty());
    picker->accept();
    CHECK(picker->error().find("not a folder") != std::string::npos);
    const auto removed = views::picker_selection(request, f.temp.path, {"removed"}, "");
    CHECK(removed.error.find("no longer exists") != std::string::npos);
    fs::create_symlink(f.temp.path / "missing", f.temp.path / "broken");
    CHECK(views::picker_selection(request, f.temp.path, {}, "broken")
              .error.find("broken symbolic link") != std::string::npos);
    request.directory = false;
    CHECK(views::picker_selection(request, f.temp.path, {}, "absent.txt")
              .error.find("existing file") != std::string::npos);
    CHECK(views::picker_selection(request, f.temp.path, {}, "folder").error.find("is a folder") !=
          std::string::npos);
    request.mode = Mode::save;
    CHECK(views::picker_selection(request, f.temp.path, {}, "").error ==
          "Enter a filename to save.");
}

TEST_CASE("browser: live name filtering composes with picker filters across all views") {
    Fixture f;
    core::PickerRequest request;
    request.filters = {{"Text", {{0, "*.txt"}}}, {"All", {{0, "*"}}}};
    auto picker = f.picker(request);
    REQUIRE(f.settle());
    for (const auto mode : {views::Browser::Mode::columns, views::Browser::Mode::grid,
                            views::Browser::Mode::details}) {
        f.browser.set_mode(mode);
        f.browser.open(f.temp.path);
        REQUIRE(f.settle());
        auto folder = f.browser.picker_folder();
        folder->select_name("one.txt");
        f.browser.set_name_filter("TWO");
        CHECK(folder->entries().empty());
        CHECK(folder->selected_names().empty());
        CHECK_FALSE(folder->cursor_entry());
        picker->set_filter(1);
        REQUIRE(folder->entries().size() == 1);
        CHECK(folder->entries()[0].name == "two.png");
        // Files arriving through the watcher obey the same filter.
        std::ofstream{f.temp.path / "two-new.txt"} << "new";
        REQUIRE(f.settle([&] { return folder->entries().size() == 2; }));
        CHECK(folder->entries().size() == 2);
        f.browser.set_name_filter("");
        CHECK(folder->entries().size() == 4);
        fs::remove(f.temp.path / "two-new.txt");
        REQUIRE(f.settle([&] { return folder->entries().size() == 3; }));
        picker->set_filter(0);
    }
}

TEST_CASE("picker: filter keyboard input and Escape do not accept or cancel the dialog") {
    Fixture f;
    auto picker = f.picker({});
    REQUIRE(f.settle());
    picker->key({.keysym = XKB_KEY_F, .text = {}, .modifiers = {.ctrl = true, .shift = true}});
    REQUIRE(f.browser.filtering());
    picker->key({.keysym = XKB_KEY_o, .text = "o"});
    picker->key({.keysym = XKB_KEY_n, .text = "n"});
    picker->key({.keysym = XKB_KEY_e, .text = "e"});
    CHECK(f.browser.name_filter() == "one");
    REQUIRE(f.browser.picker_folder()->entries().size() == 1);
    picker->key({.keysym = XKB_KEY_Return, .text = {}});
    CHECK_FALSE(f.browser.filtering());
    CHECK_FALSE(f.result);
    picker->key({.keysym = XKB_KEY_F, .text = {}, .modifiers = {.ctrl = true, .shift = true}});
    picker->key({.keysym = XKB_KEY_Escape, .text = {}});
    CHECK(f.browser.name_filter().empty());
    CHECK_FALSE(f.result);
    CHECK(f.browser.picker_folder()->entries().size() == 3);
}

TEST_CASE("picker: New Folder clears the name filter and starts renaming in all views") {
    for (const auto mode : {views::Browser::Mode::columns, views::Browser::Mode::grid,
                            views::Browser::Mode::details}) {
        Fixture f;
        core::PickerRequest request;
        request.directory = true;
        auto picker = f.picker(request);
        f.browser.set_mode(mode);
        REQUIRE(f.settle());
        picker->set_name("not-created");
        picker->accept();
        REQUIRE_FALSE(picker->error().empty());
        f.browser.set_name_filter("nothing-matches");
        f.browser.new_folder();
        REQUIRE(f.settle());
        CHECK(f.browser.name_filter().empty());
        CHECK(fs::is_directory(f.temp.path / "New Folder"));
        CHECK(f.browser.name_editor().active());
        picker->key({.keysym = XKB_KEY_Return, .text = {}});
        picker->accept();
        REQUIRE(f.result);
        CHECK(f.result->paths == std::vector<std::string>{(f.temp.path / "New Folder").string()});
    }
}
