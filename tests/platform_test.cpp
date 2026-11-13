#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/platform/applications.hpp"
#include "rummage/platform/desktop_entry.hpp"
#include "rummage/platform/exec.hpp"
#include "rummage/platform/launcher.hpp"
#include "rummage/platform/opener.hpp"
#include "temp_dir.hpp"

using rummage::core::EventLoop;
using rummage::core::WorkerPool;
using rummage::fs::MimeDatabase;
using rummage::platform::Applications;
using rummage::platform::DesktopEntry;
using rummage::platform::Environment;
using rummage::platform::Launcher;
using rummage::platform::Opener;
using rummage::platform::OpenFile;
using rummage::platform::OpenPlan;
using rummage::test::TempDir;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace platform = rummage::platform;

namespace {

using Lines = std::vector<std::vector<std::string>>;

// Runs the loop until done() holds or 5 s pass.
bool run_until(EventLoop& loop, const std::function<bool()>& done) {
    bool ok = false;
    const auto poll = loop.add_timer(1ms, 1ms, [&] {
        if (done()) {
            ok = true;
            loop.quit(0);
        }
    });
    const auto watchdog = loop.add_timer(5s, 0ms, [&loop] { loop.quit(1); });
    loop.run();
    loop.remove(poll);
    loop.remove(watchdog);
    return ok;
}

std::string read_file(const fs::path& file) {
    std::ifstream in{file};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

DesktopEntry entry_with(std::string exec) {
    DesktopEntry entry;
    entry.id = "app.desktop";
    entry.file = "/apps/app.desktop";
    entry.name = "App";
    entry.icon = "app-icon";
    entry.exec = std::move(exec);
    return entry;
}

// A fake system: config and data dirs, a bin dir in the PATH with executable scripts.
struct System {
    TempDir dir;
    Environment environment;
    std::shared_ptr<const MimeDatabase> mime = MimeDatabase::load();

    System() {
        for (const char* const sub :
             {"config", "etc", "home/applications", "share/applications", "bin"}) {
            fs::create_directories(dir.path / sub);
        }
        environment.config_dirs = {dir.path / "config", dir.path / "etc"};
        environment.data_dirs = {dir.path / "home", dir.path / "share"};
        environment.path = (dir.path / "bin").string();
    }

    // An executable script in bin; callers often need only the side effect.
    fs::path program(  // NOLINT(modernize-use-nodiscard)
        const std::string& name, std::string_view body = "exit 0\n") const {
        const fs::path file =
            dir.write("bin/" + name, std::string{"#!/bin/sh\n"} + std::string{body});
        fs::permissions(file, fs::perms::owner_all);
        return file;
    }

    // A desktop entry in a data dir ("home" or "share") running program.
    void app(const std::string& data, const std::string& id, const std::string& program,
             std::string_view extra = "") const {
        static_cast<void>(dir.write(data + "/applications/" + id,
                                    "[Desktop Entry]\nType=Application\nName=" + id +
                                        "\nExec=" + program + " %F\n" + std::string{extra}));
    }

    [[nodiscard]] std::string default_for(std::string_view type) const {
        const Applications apps = Applications::load(environment, *mime);
        const DesktopEntry* const app = apps.default_for(type, *mime);
        return app != nullptr ? app->id : std::string{"-"};
    }
};

}  // namespace

TEST_CASE("desktop entry: the keys, unescaped, Name in the best locale") {
    constexpr std::string_view text =
        "# comment\n"
        "[Desktop Entry]\n"
        "Type = Application\n"
        "Name=Viewer\n"
        "Name[it]=Visore\n"
        "Name[it_IT]=Visore italiano\n"
        "Name[de]=Betrachter\n"
        "Exec=view\\s--x %f\n"
        "TryExec=view\n"
        "Path=/tmp\n"
        "Terminal=true\n"
        "Icon=viewer\n"
        "[Desktop Action new]\n"
        "Name=Other\n"
        "Exec=other\n";
    const std::optional<DesktopEntry> entry = platform::parse_desktop_entry(text, "it_IT.UTF-8");
    REQUIRE(entry.has_value());
    CHECK(entry.value_or(DesktopEntry{}).type == "Application");
    CHECK(entry.value_or(DesktopEntry{}).name == "Visore italiano");
    CHECK(entry.value_or(DesktopEntry{}).exec == "view --x %f");
    CHECK(entry.value_or(DesktopEntry{}).try_exec == "view");
    CHECK(entry.value_or(DesktopEntry{}).working_directory == "/tmp");
    CHECK(entry.value_or(DesktopEntry{}).terminal);
    CHECK_FALSE(entry.value_or(DesktopEntry{}).hidden);

    CHECK(platform::parse_desktop_entry(text, "it_CH.UTF-8").value_or(DesktopEntry{}).name ==
          "Visore");
    CHECK(platform::parse_desktop_entry(text, "fr_FR").value_or(DesktopEntry{}).name == "Viewer");
    CHECK(platform::parse_desktop_entry(text, "C").value_or(DesktopEntry{}).name == "Viewer");
    CHECK(platform::parse_desktop_entry("[Desktop Entry]\nHidden=true\n", "")
              .value_or(DesktopEntry{})
              .hidden);
    CHECK_FALSE(platform::parse_desktop_entry("[Other]\nName=x\n", "").has_value());
}

TEST_CASE("desktop entry: string and list values") {
    CHECK(platform::unescape_value(R"(a\sb\tc\\d\ne)") == "a b\tc\\d\ne");
    CHECK(platform::split_list("image/png;image/jpeg;") ==
          std::vector<std::string>{"image/png", "image/jpeg"});
    CHECK(platform::split_list(R"(a\;b;;c)") == std::vector<std::string>{"a;b", "c"});
}

TEST_CASE("exec: quoting") {
    using Args = std::vector<std::string>;
    CHECK(platform::split_exec("app  --x %F").value_or(Args{}) == Args{"app", "--x", "%F"});
    CHECK(platform::split_exec(R"("/opt/my app/run" "a \"b\" \$c \\d")").value_or(Args{}) ==
          Args{"/opt/my app/run", R"(a "b" $c \d)"});
    CHECK(platform::split_exec("sh -c 'echo \"$1\"' x").value_or(Args{}) ==
          Args{"sh", "-c", "echo \"$1\"", "x"});
    CHECK(platform::split_exec(R"(a\ b)").value_or(Args{}) == Args{"a b"});
    CHECK_FALSE(platform::split_exec("app \"open").has_value());
    CHECK_FALSE(platform::split_exec("   ").has_value());
}

TEST_CASE("exec: field codes, one process for a list, one per file otherwise") {
    const std::vector<fs::path> files{"/d/a.jpg", "/d/b c.jpg"};
    CHECK(platform::expand_exec(entry_with("app %F"), files) ==
          Lines{{"app", "/d/a.jpg", "/d/b c.jpg"}});
    CHECK(platform::expand_exec(entry_with("app --open=%f"), files) ==
          Lines{{"app", "--open=/d/a.jpg"}, {"app", "--open=/d/b c.jpg"}});
    CHECK(platform::expand_exec(entry_with("app %U"), files) ==
          Lines{{"app", "file:///d/a.jpg", "file:///d/b%20c.jpg"}});
    CHECK(platform::expand_exec(entry_with("app %u"), files).size() == 2);
    CHECK(platform::expand_exec(entry_with("app %i --name %c %k 100%% %d %F"),
                                std::span{files}.first(1)) ==
          Lines{{"app", "--icon", "app-icon", "--name", "App", "/apps/app.desktop", "100%",
                 "/d/a.jpg"}});
    // No file code: the file is appended, one process each (as GLib does).
    CHECK(platform::expand_exec(entry_with("app --new"), files) ==
          Lines{{"app", "--new", "/d/a.jpg"}, {"app", "--new", "/d/b c.jpg"}});
    DesktopEntry no_icon = entry_with("app %i %f");
    no_icon.icon.clear();
    CHECK(platform::expand_exec(no_icon, std::span{files}.first(1)) == Lines{{"app", "/d/a.jpg"}});
    CHECK(platform::expand_exec(entry_with("app \"broken"), files).empty());
}

TEST_CASE("exec: the terminal: $TERMINAL, then xdg-terminal-exec, then the known ones") {
    System sys;
    const std::vector<std::string> argv{"hx", "/d/a.txt"};
    CHECK_FALSE(platform::in_terminal(argv, sys.environment).has_value());

    const fs::path foot = sys.program("foot");
    CHECK(platform::in_terminal(argv, sys.environment).value_or(std::vector<std::string>{}) ==
          std::vector<std::string>{foot.string(), "--", "hx", "/d/a.txt"});

    const fs::path exec = sys.program("xdg-terminal-exec");
    CHECK(platform::in_terminal(argv, sys.environment).value_or(std::vector<std::string>{}) ==
          std::vector<std::string>{exec.string(), "hx", "/d/a.txt"});

    const fs::path mine = sys.program("myterm");
    sys.environment.terminal = "myterm --big";
    CHECK(platform::in_terminal(argv, sys.environment).value_or(std::vector<std::string>{}) ==
          std::vector<std::string>{mine.string(), "--big", "-e", "hx", "/d/a.txt"});
    sys.environment.terminal = "foot";  // a known one keeps its own form
    CHECK(platform::in_terminal(argv, sys.environment).value_or(std::vector<std::string>{}) ==
          std::vector<std::string>{foot.string(), "--", "hx", "/d/a.txt"});
    sys.environment.terminal = "missing";  // not there: the next way
    CHECK(platform::in_terminal(argv, sys.environment).value_or(std::vector<std::string>{}) ==
          std::vector<std::string>{exec.string(), "hx", "/d/a.txt"});
}

TEST_CASE("exec: a terminal alone: the same one, with its options and nothing to run") {
    using Line = std::vector<std::string>;
    System sys;
    CHECK_FALSE(platform::terminal_alone(sys.environment).has_value());

    const fs::path foot = sys.program("foot");
    CHECK(platform::terminal_alone(sys.environment).value_or(Line{}) == Line{foot.string()});
    const fs::path exec = sys.program("xdg-terminal-exec");
    CHECK(platform::terminal_alone(sys.environment).value_or(Line{}) == Line{exec.string()});
    const fs::path mine = sys.program("myterm");
    sys.environment.terminal = "myterm --big";
    CHECK(platform::terminal_alone(sys.environment).value_or(Line{}) ==
          Line{mine.string(), "--big"});
    sys.environment.terminal = "missing";
    CHECK(platform::terminal_alone(sys.environment).value_or(Line{}) == Line{exec.string()});
}

TEST_CASE("applications: defaults, associations and the cache, by priority") {
    System sys;
    sys.program("viewer");
    sys.program("editor");
    sys.program("other");
    sys.app("share", "viewer.desktop", "viewer");
    sys.app("share", "editor.desktop", "editor");
    sys.app("share", "other.desktop", "other");
    static_cast<void>(sys.dir.write("share/applications/mimeinfo.cache",
                                    "[MIME Cache]\nimage/png=other.desktop;viewer.desktop;\n"
                                    "text/plain=editor.desktop;\n"));

    // The cache, in its order.
    CHECK(sys.default_for("image/png") == "other.desktop");
    // A data dir's mimeapps.list, then the user's config, then a desktop-specific one.
    static_cast<void>(sys.dir.write("share/applications/mimeapps.list",
                                    "[Default Applications]\nimage/png=editor.desktop\n"));
    CHECK(sys.default_for("image/png") == "editor.desktop");
    static_cast<void>(
        sys.dir.write("config/mimeapps.list",
                      "[Default Applications]\nimage/png=missing.desktop;viewer.desktop;\n"));
    CHECK(sys.default_for("image/png") == "viewer.desktop");  // the first installed one
    sys.environment.desktops = {"umbriel"};
    static_cast<void>(sys.dir.write("config/umbriel-mimeapps.list",
                                    "[Default Applications]\nimage/png=other.desktop\n"));
    CHECK(sys.default_for("image/png") == "other.desktop");

    // A type with no application of its own falls back to its parents: C source is plain text.
    CHECK(sys.default_for("text/x-csrc") == "editor.desktop");

    // Removed associations drop cached entries of their dir and below, not defaults.
    static_cast<void>(sys.dir.write("etc/mimeapps.list",
                                    "[Removed Associations]\ntext/plain=editor.desktop;\n"
                                    "[Added Associations]\ntext/x-csrc=viewer.desktop;\n"));
    CHECK(sys.default_for("text/plain") == "-");
    CHECK(sys.default_for("text/x-csrc") == "viewer.desktop");  // added for it
    // An alias is its canonical type.
    static_cast<void>(sys.dir.write("share/applications/mimeinfo.cache",
                                    "[MIME Cache]\nimage/x-targa=viewer.desktop;\n"));
    CHECK(sys.default_for("image/x-tga") == "viewer.desktop");
}

TEST_CASE("applications: an ID hides lower ones; hidden, broken or missing apps do not count") {
    System sys;
    sys.program("viewer");
    sys.app("share", "viewer.desktop", "viewer");
    static_cast<void>(sys.dir.write("share/applications/mimeinfo.cache",
                                    "[MIME Cache]\nimage/png=viewer.desktop;\n"));
    CHECK(sys.default_for("image/png") == "viewer.desktop");

    sys.app("home", "viewer.desktop", "viewer", "Hidden=true\n");  // the user deleted it
    CHECK(sys.default_for("image/png") == "-");
    sys.app("home", "viewer.desktop", "viewer", "TryExec=not-installed\n");
    CHECK(sys.default_for("image/png") == "-");
    sys.app("home", "viewer.desktop", "not-installed");
    CHECK(sys.default_for("image/png") == "-");
    sys.app("home", "viewer.desktop", "viewer", "Type=Link\n");
    CHECK(sys.default_for("image/png") == "-");
    // A good copy still hides the lower one's cache line: without a mimeinfo.cache of its own it
    // declares nothing (GLib does the same: measured with gio mime).
    sys.app("home", "viewer.desktop", "viewer");
    CHECK(sys.default_for("image/png") == "-");
    static_cast<void>(sys.dir.write("home/applications/mimeinfo.cache",
                                    "[MIME Cache]\nimage/png=viewer.desktop;\n"));
    CHECK(sys.default_for("image/png") == "viewer.desktop");

    // Subfolders give IDs with dashes.
    fs::create_directories(sys.dir.path / "share/applications/kde");
    sys.app("share", "kde/okular.desktop", "viewer");
    static_cast<void>(sys.dir.write("share/applications/mimeinfo.cache",
                                    "[MIME Cache]\napplication/pdf=kde-okular.desktop;\n"));
    CHECK(sys.default_for("application/pdf") == "kde-okular.desktop");
}

TEST_CASE("open plan: one process per application when it takes a list, problems listed") {
    System sys;
    sys.program("viewer");
    sys.program("hx");
    const fs::path foot = sys.program("foot");
    sys.app("share", "viewer.desktop", "viewer");
    static_cast<void>(sys.dir.write("share/applications/editor.desktop",
                                    "[Desktop Entry]\nType=Application\nName=Helix\n"
                                    "Exec=hx %f\nTerminal=true\n"));
    static_cast<void>(sys.dir.write("share/applications/mimeinfo.cache",
                                    "[MIME Cache]\nimage/png=viewer.desktop;\n"
                                    "text/plain=editor.desktop;\n"));
    const Applications apps = Applications::load(sys.environment, *sys.mime);
    const std::vector<OpenFile> files{{.path = "/d/a.png", .mime = "image/png"},
                                      {.path = "/d/b.txt", .mime = "text/plain"},
                                      {.path = "/e/c.png", .mime = "image/png"},
                                      {.path = "/d/d.zip", .mime = "application/zip"}};
    const OpenPlan plan = platform::plan_open(files, apps, *sys.mime, sys.environment);
    REQUIRE(plan.launches.size() == 2);
    CHECK(plan.launches[0].app_name == "viewer.desktop");
    CHECK(plan.launches[0].argv ==
          std::vector<std::string>{(sys.dir.path / "bin/viewer").string(), "/d/a.png", "/e/c.png"});
    CHECK(plan.launches[0].directory == "/d");
    CHECK(plan.launches[0].files == 2);
    CHECK(plan.launches[1].app_name == "Helix");
    CHECK(plan.launches[1].argv == std::vector<std::string>{foot.string(), "--", "hx", "/d/b.txt"});
    CHECK(plan.files == 3);
    CHECK(plan.first_name == "a.png");
    CHECK(plan.problems == std::vector<std::string>{"No application opens application/zip"});
}

TEST_CASE("launcher: exit status, errors, a clean environment and signal mask") {
    TempDir dir;
    EventLoop loop;
    Launcher launcher{loop};
    std::optional<Launcher::Exit> exit;
    const std::vector<std::string> env{"RUMMAGE_TEST_VAR=hello"};
    // The child sees the added variable, runs in the folder given, and blocks no signal (rummage
    // blocks SIGINT and SIGTERM for its signalfd).
    REQUIRE_FALSE(launcher.spawn({"sh", "-c",
                                  "echo \"$RUMMAGE_TEST_VAR $(pwd)\" > out; "
                                  "grep SigBlk /proc/self/status >> out; exit 3"},
                                 dir.path, env, [&](Launcher::Exit e) { exit = e; }));
    CHECK(launcher.running() == 1);
    REQUIRE(run_until(loop, [&] { return exit.has_value(); }));
    CHECK(exit.value_or(Launcher::Exit{}).status == 3);
    CHECK(exit.value_or(Launcher::Exit{}).signal == 0);
    CHECK(launcher.running() == 0);
    CHECK(read_file(dir.path / "out") ==
          "hello " + dir.path.string() + "\nSigBlk:\t0000000000000000\n");

    exit.reset();
    REQUIRE_FALSE(
        launcher.spawn({"sh", "-c", "kill -TERM $$"}, {}, {}, [&](Launcher::Exit e) { exit = e; }));
    REQUIRE(run_until(loop, [&] { return exit.has_value(); }));
    CHECK(exit.value_or(Launcher::Exit{}).signal == SIGTERM);

    const std::error_code error =
        launcher.spawn({"rummage-no-such-program"}, {}, {}, [](Launcher::Exit /*e*/) {});
    CHECK(error == std::errc::no_such_file_or_directory);
    CHECK(launcher.running() == 0);
}

TEST_CASE("opener: the files reach their application, and what happened is reported") {
    System sys;
    const fs::path log = sys.dir.path / "args";
    sys.program("viewer", R"(for a in "$@"; do echo "$a"; done > )" + log.string() + "\n");
    sys.program("broken", "exit 1\n");
    sys.app("share", "viewer.desktop", "viewer");
    sys.app("share", "broken.desktop", "broken");
    static_cast<void>(sys.dir.write("share/applications/mimeinfo.cache",
                                    "[MIME Cache]\nimage/jpeg=viewer.desktop;\n"
                                    "text/plain=broken.desktop;\n"));
    fs::create_directories(sys.dir.path / "photos");
    // Types left to detection, as the views leave them until needed.
    static_cast<void>(sys.dir.write("photos/a.jpg", "x"));
    static_cast<void>(sys.dir.write("photos/b.jpg", "x"));

    EventLoop loop;
    WorkerPool pool{loop, 2};
    std::vector<std::pair<std::string, bool>> reports;
    Opener opener{
        loop, pool, sys.mime, sys.environment,
        [&](const std::string& message, bool error) { reports.emplace_back(message, error); }};
    using Kind = rummage::fs::EntryKind;
    opener.open(sys.dir.path / "photos", {{.name = "a.jpg", .kind = Kind::file, .size = 1},
                                          {.name = "b.jpg", .kind = Kind::file, .size = 1},
                                          {.name = "sub", .kind = Kind::directory}});
    REQUIRE(
        run_until(loop, [&] { return !opener.planning() && opener.launcher().running() == 0; }));
    REQUIRE(reports.size() == 1);
    CHECK(reports[0] ==
          std::pair<std::string, bool>{"Opening 2 files with viewer.desktop…", false});
    const std::string photos = (sys.dir.path / "photos").string();
    CHECK(read_file(log) == photos + "/a.jpg\n" + photos + "/b.jpg\n");

    // An application that quits at once with an error is reported.
    reports.clear();
    opener.open(sys.dir.path / "photos",
                {{.name = "c.txt", .kind = Kind::file, .mime = sys.mime->find("text/plain")}});
    REQUIRE(run_until(loop, [&] { return reports.size() == 2; }));
    CHECK(reports[1] ==
          std::pair<std::string, bool>{"broken.desktop quit at once with status 1", true});

    // An empty file opens as its name says (GLib calls it application/x-zerosize): a new notes.txt
    // goes to the text application, and so do its "Open With" choices.
    reports.clear();
    static_cast<void>(sys.dir.write("photos/empty.txt", ""));
    static_cast<void>(sys.dir.write("photos/empty", ""));
    opener.open(sys.dir.path / "photos", {{.name = "empty.txt", .kind = Kind::file, .size = 0}});
    REQUIRE(run_until(loop, [&] { return reports.size() == 2; }));
    CHECK(reports[0] ==
          std::pair<std::string, bool>{"Opening empty.txt with broken.desktop…", false});
    std::vector<platform::AppChoice> choices;
    bool answered = false;
    opener.choices(sys.dir.path / "photos", {.name = "empty.txt", .kind = Kind::file, .size = 0},
                   [&](std::vector<platform::AppChoice> found) {
                       choices = std::move(found);
                       answered = true;
                   });
    REQUIRE(run_until(loop, [&] { return answered; }));
    REQUIRE(choices.size() == 1);
    CHECK(choices[0].id == "broken.desktop");
    // Without a type in its name, an empty file is still empty.
    reports.clear();
    opener.open(sys.dir.path / "photos", {{.name = "empty", .kind = Kind::file, .size = 0}});
    REQUIRE(run_until(loop, [&] { return !reports.empty(); }));
    CHECK(reports[0] ==
          std::pair<std::string, bool>{"No application opens application/x-zerosize", true});

    // No application at all.
    reports.clear();
    opener.open(sys.dir.path / "photos",
                {{.name = "d.zip", .kind = Kind::file, .mime = sys.mime->find("application/zip")}});
    REQUIRE(run_until(loop, [&] { return !reports.empty(); }));
    CHECK(reports[0] == std::pair<std::string, bool>{"No application opens application/zip", true});
}

TEST_CASE("opener: a terminal in a folder, and a folder with a chosen application") {
    System sys;
    const fs::path log = sys.dir.path / "where";
    sys.program("editor", R"(for a in "$@"; do echo "$a"; done > )" + log.string() + "\n");
    sys.app("share", "editor.desktop", "editor");
    fs::create_directories(sys.dir.path / "photos" / "sub");

    EventLoop loop;
    WorkerPool pool{loop, 2};
    std::vector<std::pair<std::string, bool>> reports;
    Opener opener{
        loop, pool, sys.mime, sys.environment,
        [&](const std::string& message, bool error) { reports.emplace_back(message, error); }};
    opener.open_terminal(sys.dir.path / "photos");
    REQUIRE(run_until(loop, [&] { return !reports.empty(); }));
    CHECK(reports[0] == std::pair<std::string, bool>{"No terminal found: set $TERMINAL", true});

    // The terminal starts in the folder: the fake one writes where it is.
    sys.program("foot", "pwd > " + log.string() + "\n");
    reports.clear();
    opener.open_terminal(sys.dir.path / "photos" / "sub");
    REQUIRE(run_until(loop, [&] {
        return reports.size() == 1 && opener.launcher().running() == 0 && !opener.planning();
    }));
    CHECK(reports[0] == std::pair<std::string, bool>{"Opening sub with foot…", false});
    CHECK(read_file(log) == (sys.dir.path / "photos" / "sub").string() + "\n");

    // Open With on a folder: the application gets it (without one, folders are left out).
    reports.clear();
    using Kind = rummage::fs::EntryKind;
    opener.open(sys.dir.path / "photos", {{.name = "sub", .kind = Kind::directory}},
                "editor.desktop");
    REQUIRE(run_until(loop, [&] {
        return reports.size() == 1 && opener.launcher().running() == 0 && !opener.planning();
    }));
    CHECK(reports[0] == std::pair<std::string, bool>{"Opening sub with editor.desktop…", false});
    CHECK(read_file(log) == (sys.dir.path / "photos" / "sub").string() + "\n");
}

TEST_CASE("applications: every one for a type, in GLib's order, the default first to choose") {
    System sys;
    for (const char* const name : {"viewer", "editor", "other", "gone"}) {
        sys.program(name);
        sys.app("share", std::string{name} + ".desktop", name);
    }
    static_cast<void>(
        sys.dir.write("share/applications/mimeinfo.cache",
                      "[MIME Cache]\nimage/png=other.desktop;viewer.desktop;missing.desktop;\n"
                      "text/plain=editor.desktop;viewer.desktop;\n"
                      "text/x-csrc=gone.desktop;\n"));
    static_cast<void>(sys.dir.write("config/mimeapps.list",
                                    "[Default Applications]\nimage/png=viewer.desktop\n"
                                    "[Added Associations]\nimage/png=editor.desktop;\n"
                                    "[Removed Associations]\ntext/x-csrc=gone.desktop;\n"));
    const Applications apps = Applications::load(sys.environment, *sys.mime);
    const auto ids = [&](std::string_view type, bool parents) {
        std::vector<std::string> out;
        for (const DesktopEntry* const app : apps.all_for(type, *sys.mime, parents)) {
            out.push_back(app->id);
        }
        return out;
    };
    using Ids = std::vector<std::string>;
    // Added associations first, then the cache; not installed ones left out; no defaults.
    CHECK(ids("image/png", true) == Ids{"editor.desktop", "other.desktop", "viewer.desktop"});
    // C source: its own (removed: none), then plain text's.
    CHECK(ids("text/x-csrc", false).empty());
    CHECK(ids("text/x-csrc", true) == Ids{"editor.desktop", "viewer.desktop"});

    // The choices: the default first, each application once.
    const std::vector<platform::AppChoice> choices =
        platform::choices_for("image/png", apps, *sys.mime);
    REQUIRE(choices.size() == 3);
    CHECK(choices[0].id == "viewer.desktop");
    CHECK(choices[0].is_default);
    CHECK(choices[1].id == "editor.desktop");
    CHECK_FALSE(choices[1].is_default);
    CHECK(choices[2].id == "other.desktop");

    // Open With: every file goes to the chosen application, whatever its type.
    const std::vector<OpenFile> files{{.path = "/d/a.png", .mime = "image/png"},
                                      {.path = "/d/b.zip", .mime = "application/zip"}};
    const OpenPlan plan =
        platform::plan_open(files, apps, *sys.mime, sys.environment, "other.desktop");
    REQUIRE(plan.launches.size() == 1);
    CHECK(plan.launches[0].app_id == "other.desktop");
    CHECK(plan.files == 2);
    CHECK(platform::plan_open(files, apps, *sys.mime, sys.environment, "nope.desktop").problems ==
          std::vector<std::string>{"nope.desktop is not installed"});
}
