#include "rummage/core/actions.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/config.hpp"
#include "rummage/platform/action_handoff.hpp"
#include "temp_dir.hpp"

using namespace std::chrono_literals;
using rummage::core::Action;
using rummage::core::action_arguments;
using rummage::core::action_matches;
using rummage::core::action_registry;
using rummage::core::ActionContext;

TEST_CASE("actions: arguments preserve filename bytes and selection boundaries") {
    Action action{.id = "convert",
                  .label = "Convert",
                  .argv = {"tool", "{files}", "{directory}"},
                  .mime = {"image/*"},
                  .selection = Action::Selection::many};
    ActionContext context{.directory = "/photos",
                          .targets = {{"/photos/a b.png", "image/png"},
                                      {"/photos/$(touch bad);'\n.png", "image/png"}}};
    REQUIRE(action_matches(action, context));
    CHECK(action_arguments(action, context) ==
          std::vector<std::string>{"tool", "/photos/a b.png", "/photos/$(touch bad);'\n.png",
                                   "/photos"});
    context.targets.push_back({"/photos/readme", "text/plain"});
    CHECK_FALSE(action_matches(action, context));
    CHECK(action_arguments(action, context).empty());
    context.targets.clear();
    CHECK_FALSE(action_matches(action, context));
    action.selection = Action::Selection::background;
    action.argv = {"tool", "{directory}"};
    action.mime = {"inode/directory"};
    CHECK(action_matches(action, context));
    context.trash = true;
    CHECK_FALSE(action_matches(action, context));
}

TEST_CASE("actions: bundled extraction requires only supported archive types") {
    const auto actions = action_registry();
    REQUIRE(actions.size() == 3);
    ActionContext context{.directory = "/tmp", .targets = {{"/tmp/a.zip", "application/zip"}}};
    CHECK(action_matches(actions[1], context));
    CHECK(actions[1].kind == Action::Kind::extract_here);
    CHECK(actions[2].kind == Action::Kind::extract_to);
    context.targets.push_back({"/tmp/b", "inode/directory"});
    CHECK_FALSE(action_matches(actions[1], context));
    CHECK(action_matches(actions[0], context));
    auto replacement = actions[1];
    replacement.enabled = false;
    CHECK_FALSE(action_matches(action_registry(std::vector{replacement})[1], context));
}

TEST_CASE("actions: bzip2 tar canonical MIME and legacy alias both expose extraction") {
    for (const auto* mime :
         {"application/x-bzip2-compressed-tar", "application/x-bzip-compressed-tar"}) {
        for (const auto* name : {"archive.tar.bz2", "archive.tbz2"}) {
            const ActionContext context{.directory = "/tmp", .targets = {{name, mime}}};
            CHECK(action_matches(action_registry()[1], context));
            CHECK(action_matches(action_registry()[2], context));
        }
    }
}

TEST_CASE("actions: config validates the entire action set") {
    const std::string valid = R"(
[[actions]]
id = "upscale"
label = "Upscale Image"
argv = ["upscaler", "{file}"]
mime = ["image/*"]
selection = "one"
execution = "handoff"
[keybindings]
"action:upscale" = "Ctrl+Alt+u"
)";
    const auto result = rummage::core::parse_config(valid, "actions.toml");
    REQUIRE(result.problems.empty());
    REQUIRE(result.config.actions.size() == 1);
    CHECK(result.config.actions.front().placement == Action::Placement::actions);
    CHECK(result.config.keybindings.at("action:upscale") == "Ctrl+Alt+u");
    for (const auto* invalid :
         {"actions = 1", "[[actions]]\nid = 'incomplete'",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool','{unknown}']",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool','prefix{files}']",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool','{file}']\nselection='many'",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool','{files}']\nselection='background'",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool']\nexecution='job'",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool']\nplacement='toolbar'",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool']\nplacement=true",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool']\nconfirm=true",
          "[[actions]]\nid='copy_location'\nlabel='Bad'\nargv=['tool']",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool',7]",
          "[[actions]]\nid='bad'\nlabel='Bad'\nargv=['tool']\nmime=['*/*']"}) {
        CAPTURE(invalid);
        const auto bad = rummage::core::parse_config(invalid, "actions.toml");
        CHECK_FALSE(bad.problems.empty());
        CHECK_FALSE(bad.actions_valid);
        CHECK(bad.config.actions.empty());
    }
    const auto preset = rummage::core::parse_config(
        "[[actions]]\nid='extract_here'\nenabled=false\n"
        "[[actions]]\nid='extract_to'\nargv=['other-archiver','{files}']",
        "actions.toml");
    REQUIRE(preset.problems.empty());
    const auto registry = action_registry(preset.config.actions);
    CHECK_FALSE(registry[1].enabled);
    CHECK(registry[2].label == "Extract To…");
    CHECK(registry[2].argv == std::vector<std::string>{"other-archiver", "{files}"});
    CHECK(registry[2].selection == Action::Selection::many);
    CHECK(registry[2].placement == Action::Placement::menu);
    const auto duplicate = rummage::core::parse_config(
        valid + "\n[[actions]]\nid='upscale'\nlabel='Again'\nargv=['tool']", "actions.toml");
    CHECK_FALSE(duplicate.actions_valid);
    CHECK(duplicate.config.actions.empty());
}

TEST_CASE("actions: config supports menu placement and explicit submenu placement") {
    const auto result = rummage::core::parse_config(R"(
[[actions]]
id = "compress"
label = "Compress…"
argv = ["file-roller", "--add", "{files}"]
selection = "many"
placement = "menu"
[[actions]]
id = "goblin"
label = "Consult the File Goblin"
argv = ["notify-send", "Quack"]
placement = "actions"
[[actions]]
id = "extract_here"
placement = "actions"
)",
                                                    "actions.toml");
    REQUIRE(result.problems.empty());
    REQUIRE(result.config.actions.size() == 3);
    CHECK(result.config.actions[0].placement == Action::Placement::menu);
    CHECK(result.config.actions[1].placement == Action::Placement::actions);
    const auto registry = action_registry(result.config.actions);
    CHECK(registry[0].placement == Action::Placement::menu);
    CHECK(registry[1].placement == Action::Placement::actions);
    CHECK(registry[2].placement == Action::Placement::menu);
}

TEST_CASE("actions: handoff passes arguments and cwd literally and reports failures") {
    rummage::test::TempDir dir;
    rummage::core::EventLoop loop;
    std::vector<std::string> errors;
    rummage::platform::ActionHandoff handoff{loop,
                                             rummage::platform::Environment::from_environment(),
                                             [&](std::string text, bool error) {
                                                 if (error) {
                                                     errors.push_back(std::move(text));
                                                     loop.quit(0);
                                                 }
                                             }};
    Action action{.id = "record",
                  .label = "Record",
                  .argv = {"sh", "-c", "printf '%s\\0' \"$PWD\" \"$@\" > arguments; exit 7",
                           "rummage-test", "{files}"},
                  .mime = {},
                  .selection = Action::Selection::many};
    const std::string tricky = (dir.path / "a b;'$(touch BAD)\n.png").string();
    const ActionContext context{.directory = dir.path, .targets = {{tricky, "image/png"}}};
    handoff.run(action, context);
    const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
    REQUIRE(loop.run() == 0);
    loop.remove(timeout);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().find("status 7") != std::string::npos);
    std::ifstream input{dir.path / "arguments", std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    CHECK(bytes == dir.path.string() + '\0' + tricky + '\0');
    CHECK_FALSE(std::filesystem::exists(dir.path / "BAD"));
    action.argv = {"rummage-nonexistent-action-program", "{files}"};
    handoff.run(action, context);
    REQUIRE(errors.size() == 2);
    CHECK(errors.back().find("not installed") != std::string::npos);
}

TEST_CASE("actions: pending activation cannot use a destroyed handoff") {
    rummage::core::EventLoop loop;
    std::function<void(std::string)> pending;
    unsigned reports = 0;
    {
        rummage::platform::ActionHandoff handoff{
            loop, rummage::platform::Environment::from_environment(),
            [&](std::string, bool) { ++reports; }, [&](auto done) { pending = std::move(done); }};
        const Action action{.id = "noop",
                            .label = "Noop",
                            .argv = {"sh", "-c", "exit 0"},
                            .mime = {},
                            .selection = Action::Selection::background};
        handoff.run(action, {.directory = "/tmp", .targets = {}});
    }
    REQUIRE(pending);
    pending("token");
    CHECK(reports == 0);
}

TEST_CASE("actions: destination picker config and literal argument expansion") {
    const auto result = rummage::core::parse_config(R"(
[[actions]]
id = "compress"
label = "Compress to ZIP…"
argv = ["7z", "a", "-tzip", "{destination}", "--", "{files}"]
selection = "many"
destination = "file"
destination_name = "Archive.zip"
[[actions]]
id = "copy-to"
label = "Copy To…"
argv = ["cp", "--", "{files}", "{destination}"]
selection = "many"
destination = "directory"
)",
                                                    "picker-actions.toml");
    REQUIRE(result.problems.empty());
    REQUIRE(result.config.actions.size() == 2);
    const auto& save = result.config.actions[0];
    CHECK(save.destination == Action::Destination::file);
    CHECK(save.destination_name == "Archive.zip");
    CHECK(result.config.actions[1].destination == Action::Destination::directory);
    ActionContext context{.directory = "/tmp",
                          .targets = {{"/tmp/a b", "text/plain"}, {"/tmp/@list", "text/plain"}}};
    CHECK(action_matches(save, context));
    CHECK(action_arguments(save, context).empty());  // no program launch before a choice
    const std::string chosen = "/tmp/quote'\n$(touch BAD).zip";
    CHECK(action_arguments(save, context, chosen) ==
          std::vector<std::string>{"7z", "a", "-tzip", chosen, "--", "/tmp/a b", "/tmp/@list"});
    for (const auto* fields : {
             "argv=['tool','{destination}']",
             "argv=['tool']\ndestination='file'",
             "argv=['tool','{destination}']\ndestination='open'",
             "argv=['tool','{destination}']\ndestination=true",
             "argv=['tool','{destination}']\ndestination='none'",
             "argv=['tool','--output={destination}']\ndestination='file'",
             "argv=['{destination}']\ndestination='file'",
             "argv=['tool']\ndestination_name='out.zip'",
             "argv=['tool','{destination}']\ndestination='directory'\ndestination_name='out.zip'",
             "argv=['tool','{destination}']\ndestination='file'\ndestination_name='../out.zip'",
             "argv=['tool','{destination}']\ndestination='file'\ndestination_name='..'",
             "argv=['tool','{destination}']\ndestination='file'\ndestination_name=123",
         }) {
        CAPTURE(std::string{fields});
        const auto invalid = rummage::core::parse_config(
            "[[actions]]\nid='bad'\nlabel='Bad'\n" + std::string{fields}, "actions.toml");
        CHECK_FALSE(invalid.actions_valid);
        CHECK_FALSE(invalid.problems.empty());
    }
    CHECK_FALSE(rummage::core::parse_config(
                    "[[actions]]\nid='extract_here'\ndestination='directory'", "actions.toml")
                    .actions_valid);
}

TEST_CASE("actions: chosen destinations launch once with captured selection and cwd") {
    rummage::test::TempDir dir;
    rummage::core::EventLoop loop;
    using Handoff = rummage::platform::ActionHandoff;
    Handoff::DirectoryChosen pending;
    rummage::core::PickerRequest request;
    std::vector<std::string> errors;
    Handoff handoff{loop,
                    rummage::platform::Environment::from_environment(),
                    [&](std::string text, bool error) {
                        if (error) {
                            errors.push_back(std::move(text));
                            loop.quit(0);
                        }
                    },
                    {},
                    {},
                    [&](const auto& picker, auto chosen) {
                        request = picker;
                        pending = chosen;
                    }};
    Action action{.id = "record",
                  .label = "Save Output",
                  .argv = {"sh", "-c", "printf '%s\\0' \"$PWD\" \"$@\" > arguments; exit 7",
                           "rummage-test", "{destination}", "{files}"},
                  .mime = {},
                  .selection = Action::Selection::many,
                  .destination = Action::Destination::file,
                  .destination_name = "Archive.zip"};
    ActionContext context{
        .directory = dir.path,
        .targets = {{dir.path / "one file", "text/plain"}, {dir.path / "two files", "text/plain"}}};
    SUBCASE("save file") {}
    SUBCASE("choose directory") {
        action.destination = Action::Destination::directory;
        action.destination_name.clear();
    }
    handoff.run(action, context);
    REQUIRE(pending);
    CHECK_FALSE(std::filesystem::exists(dir.path / "arguments"));
    CHECK(request.folder == dir.path.string());
    CHECK(request.title == "Save Output");
    CHECK(request.name == action.destination_name);
    const bool directory = action.destination == Action::Destination::directory;
    CHECK(request.directory == directory);
    CHECK(request.mode == (directory ? rummage::core::PickerRequest::Mode::open
                                     : rummage::core::PickerRequest::Mode::save));
    const auto destination = dir.path / "chosen '\n$(touch BAD)";
    if (directory)
        std::filesystem::create_directory(destination);
    const auto original = context;
    context.directory = "/";
    context.targets.clear();  // navigation while the picker is open cannot change the action
    pending(destination);
    const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
    REQUIRE(loop.run() == 0);
    loop.remove(timeout);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().find("status 7") != std::string::npos);
    std::ifstream input{dir.path / "arguments", std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    CHECK(bytes == dir.path.string() + '\0' + destination.string() + '\0' +
                       original.targets[0].path.string() + '\0' +
                       original.targets[1].path.string() + '\0');
    CHECK_FALSE(std::filesystem::exists(dir.path / "BAD"));
    if (!directory)
        CHECK_FALSE(std::filesystem::exists(destination));  // picker creates no output
}

TEST_CASE("actions: cancellation invalid destinations and destroyed handoffs do not launch") {
    rummage::test::TempDir dir;
    rummage::core::EventLoop loop;
    using Handoff = rummage::platform::ActionHandoff;
    Handoff::DirectoryChosen pending;
    unsigned reports = 0;
    Action action{.id = "record",
                  .label = "Record",
                  .argv = {"sh", "-c", "touch launched", "rummage-test", "{destination}"},
                  .mime = {},
                  .selection = Action::Selection::background,
                  .destination = Action::Destination::file};
    {
        Handoff handoff{loop,
                        rummage::platform::Environment::from_environment(),
                        [&](std::string, bool) { ++reports; },
                        {},
                        {},
                        [&](const auto&, auto chosen) { pending = chosen; }};
        handoff.run(action, {.directory = dir.path, .targets = {}});
        REQUIRE(pending);
        SUBCASE("cancel") {
            pending(std::nullopt);
            CHECK(reports == 0);
        }
        SUBCASE("relative path") {
            pending("relative.zip");
            CHECK(reports == 1);
        }
        SUBCASE("missing parent") {
            pending(dir.path / "missing" / "out.zip");
            CHECK(reports == 1);
        }
        SUBCASE("wrong type") {
            pending(dir.path);
            CHECK(reports == 1);
        }
        SUBCASE("missing application") {
            pending = {};
            action.argv[0] = "rummage-nonexistent-program";
            handoff.run(action, {.directory = dir.path, .targets = {}});
            CHECK_FALSE(pending);
            CHECK(reports == 1);
        }
        SUBCASE("destroy while choosing") {}
    }
    const auto previous = reports;
    if (pending)
        pending(dir.path / "later.zip");
    CHECK(reports == previous);
    CHECK_FALSE(std::filesystem::exists(dir.path / "launched"));
}

TEST_CASE("actions: completion notification config is opt in and external only") {
    CHECK_FALSE(Action{}.notify_on_completion);
    for (const bool enabled : {false, true}) {
        const auto result = rummage::core::parse_config(
            "[[actions]]\nid='compress'\nlabel='Compress'\nargv=['7z']\nnotify_on_completion=" +
                std::string{enabled ? "true" : "false"},
            "notify.toml");
        REQUIRE(result.actions_valid);
        REQUIRE(result.config.actions.size() == 1);
        CHECK(result.config.actions[0].notify_on_completion == enabled);
    }
    for (const auto* value : {"'true'", "1", "[]"}) {
        const auto result = rummage::core::parse_config(
            "[[actions]]\nid='compress'\nlabel='Compress'\nargv=['7z']\nnotify_on_completion=" +
                std::string{value},
            "notify.toml");
        CHECK_FALSE(result.actions_valid);
    }
    CHECK_FALSE(rummage::core::parse_config(
                    "[[actions]]\nid='extract_to'\nnotify_on_completion=true", "notify.toml")
                    .actions_valid);
    CHECK(rummage::core::parse_config(
              "[[actions]]\nid='extract_to'\nargv=['tool','{files}']\nnotify_on_completion=true",
              "notify.toml")
              .actions_valid);
}

TEST_CASE("actions: completion notifications follow exit status and signals") {
    rummage::test::TempDir dir;
    rummage::core::EventLoop loop;
    Action action{.id = "complete",
                  .label = "Compress <test> & files",
                  .argv = {"sh", "-c", "printf complete > output; exit 0"},
                  .mime = {},
                  .selection = Action::Selection::background,
                  .notify_on_completion = true};
    bool failed = false;
    std::string expected = "Completed successfully";
    SUBCASE("success") {}
    SUBCASE("nonzero exit") {
        action.argv.back() = "exit 7";
        failed = true;
        expected = "Program exited with status 7";
    }
    SUBCASE("terminated by signal") {
        action.argv.back() = "kill -TERM $$";
        failed = true;
        expected = "Program stopped by signal 15";
    }
    SUBCASE("disabled keeps existing error reporting without notification") {
        action.notify_on_completion = false;
        action.argv.back() = "exit 7";
        failed = true;
        expected = "Program exited with status 7";
    }
    std::vector<std::pair<std::string, bool>> reports;
    std::vector<std::string> notifications;
    rummage::platform::ActionHandoff handoff{
        loop,
        rummage::platform::Environment::from_environment(),
        [&](std::string text, bool error) {
            reports.emplace_back(std::move(text), error);
            if (error && !action.notify_on_completion)
                loop.quit(0);
        },
        {},
        {},
        {},
        [&](const std::string& title, const std::string& body, bool error) {
            notifications.push_back(title);
            CHECK(body == expected);
            CHECK(error == failed);
            if (!failed)
                CHECK(std::filesystem::exists(dir.path / "output"));
            loop.quit(0);
        }};
    handoff.run(action, {.directory = dir.path, .targets = {}});
    CHECK(notifications.empty());
    const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
    REQUIRE(loop.run() == 0);
    loop.remove(timeout);
    REQUIRE(reports.size() == 2);
    CHECK(reports.back().first == action.label + ": " + expected);
    CHECK(reports.back().second == failed);
    if (action.notify_on_completion) {
        REQUIRE(notifications.size() == 1);
        CHECK(notifications.front() ==
              std::string{failed ? "Failed: " : "Completed: "} + action.label);
    } else
        CHECK(notifications.empty());
}

TEST_CASE("actions: cancelled picker and launch failures do not notify completion") {
    rummage::test::TempDir dir;
    rummage::core::EventLoop loop;
    using Handoff = rummage::platform::ActionHandoff;
    Handoff::DirectoryChosen pending;
    unsigned notifications = 0;
    unsigned errors = 0;
    Handoff handoff{loop,
                    rummage::platform::Environment::from_environment(),
                    [&](std::string, bool error) {
                        if (error)
                            ++errors;
                    },
                    {},
                    {},
                    [&](const auto&, auto chosen) { pending = chosen; },
                    [&](const auto&, const auto&, bool) { ++notifications; }};
    Action action{.id = "compress",
                  .label = "Compress",
                  .argv = {"sh", "-c", "exit 0", "test", "{destination}"},
                  .mime = {},
                  .selection = Action::Selection::background,
                  .destination = Action::Destination::file,
                  .notify_on_completion = true};
    SUBCASE("cancel") {
        handoff.run(action, {.directory = dir.path, .targets = {}});
        REQUIRE(pending);
        pending(std::nullopt);
        CHECK(errors == 0);
    }
    SUBCASE("missing executable") {
        action.argv.front() = "rummage-nonexistent-program";
        handoff.run(action, {.directory = dir.path, .targets = {}});
        CHECK_FALSE(pending);
        CHECK(errors == 1);
    }
    SUBCASE("spawn error") {
        action.destination = Action::Destination::none;
        action.argv = {"sh", "-c", "exit 0"};
        handoff.run(action, {.directory = dir.path / "missing", .targets = {}});
        CHECK(errors == 1);
    }
    CHECK(notifications == 0);
}
