#include "rummage/platform/archives.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "rummage/core/event_loop.hpp"
#include "rummage/platform/action_handoff.hpp"
#include "temp_dir.hpp"

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rummage::core::ActionContext;
using rummage::platform::ActionHandoff;
using rummage::platform::ArchiveBackend;

std::string read(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

struct Fixture {
    rummage::test::TempDir dir;
    rummage::core::EventLoop loop;
    rummage::platform::Environment environment;
    fs::path bin = dir.path / "bin";
    fs::path destination = dir.path / "destination";
    std::vector<std::string> errors;
    std::vector<std::string> messages;

    Fixture() {
        fs::create_directory(bin);
        fs::create_directory(destination);
        environment.path = bin.string();
    }

    bool install(const std::string& program) {
        const auto real = rummage::platform::find_program(
            program, rummage::platform::Environment::from_environment().path);
        if (!real) {
            MESSAGE(program << " is not installed; integration test skipped");
            return false;
        }
        fs::create_symlink(*real, bin / program);
        return true;
    }

    void fake(const std::string& name, const std::string& body) {
        const auto shell = rummage::platform::find_program(
            "sh", rummage::platform::Environment::from_environment().path);
        REQUIRE(shell);
        const auto program = bin / name;
        std::ofstream{program} << "#!" << shell->string() << "\n" << body;
        fs::permissions(program, fs::perms::owner_all);
    }

    auto report() {
        return [this](std::string message, bool error) {
            (error ? errors : messages).push_back(std::move(message));
        };
    }

    fs::path archive(const std::string& fixture, const std::string& name) {
        const auto out = dir.path / name;
        fs::copy_file(fs::path{RUMMAGE_TEST_DATA} / "archives" / fixture, out);
        return out;
    }

    bool until(const std::function<bool()>& ready) {
        bool done = false;
        const auto poll = loop.add_timer(1ms, 1ms, [&] {
            if (ready()) {
                done = true;
                loop.quit(0);
            }
        });
        const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
        loop.run();
        loop.remove(poll);
        loop.remove(timeout);
        return done;
    }

    bool wait(unsigned count = 1) {
        bool done = false;
        const auto poll = loop.add_timer(1ms, 1ms, [&] {
            unsigned completed = 0;
            for (const auto& message : messages) {
                if (message.starts_with("Extracted to "))
                    ++completed;
            }
            if (!errors.empty() || completed >= count) {
                done = true;
                loop.quit(0);
            }
        });
        const auto timeout = loop.add_timer(5s, 0ms, [&] { loop.quit(1); });
        loop.run();
        loop.remove(poll);
        loop.remove(timeout);
        return done;
    }
};
}  // namespace

TEST_CASE("archives: format selection only offers installed capable programs") {
    Fixture f;
    const auto program = f.dir.write("fake", "program");
    fs::permissions(program, fs::perms::owner_all);
    const auto add = [&](const std::string& name) { fs::create_symlink(program, f.bin / name); };
    const rummage::core::ActionTarget zip{f.dir.path / "a.zip", "application/zip"};
    const rummage::core::ActionTarget gz{f.dir.path / "a.gz", "application/gzip"};
    const rummage::core::ActionTarget tgz{f.dir.path / "a.tar.gz", "application/gzip"};
    CHECK_FALSE(rummage::platform::archive_backend(zip, f.environment));
    add("gzip");
    CHECK(rummage::platform::archive_backend(gz, f.environment)->kind ==
          ArchiveBackend::Kind::gzip);
    CHECK_FALSE(rummage::platform::archive_backend(tgz, f.environment));
    CHECK_FALSE(rummage::platform::archive_backend(zip, f.environment));
    add("7z");
    CHECK(rummage::platform::archive_backend(zip, f.environment)->kind ==
          ArchiveBackend::Kind::seven_zip);
    CHECK(rummage::platform::archive_backend(tgz, f.environment)->tar_layer);
    add("unzip");
    CHECK(rummage::platform::archive_backend(zip, f.environment)->kind ==
          ArchiveBackend::Kind::seven_zip);
    add("tar");
    CHECK(rummage::platform::archive_backend(tgz, f.environment)->kind ==
          ArchiveBackend::Kind::tar);
    add("file-roller");
    CHECK(rummage::platform::archive_backend(zip, f.environment)->kind ==
          ArchiveBackend::Kind::seven_zip);
    CHECK(rummage::platform::archive_backend(tgz, f.environment)->kind ==
          ArchiveBackend::Kind::file_roller);
}

TEST_CASE("archives: unzip handles literal wildcards and never overwrites destination folders") {
    Fixture f;
    if (!f.install("unzip"))
        return;
    const auto archive = f.archive("sample.zip", "a*[1]? 'quoted'.zip");
    const auto name = rummage::platform::archive_output_name(archive);
    fs::create_directory(f.dir.path / name);
    std::ofstream{f.dir.path / name / "keep"} << "existing";
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    const ActionContext context{.directory = f.dir.path, .targets = {{archive, "application/zip"}}};
    REQUIRE(handoff.available(rummage::core::action_registry()[1], context));
    handoff.run(rummage::core::action_registry()[1], context);
    REQUIRE(f.wait());
    CHECK(f.errors.empty());
    CHECK(read(f.dir.path / name / "keep") == "existing");
    CHECK(read(f.dir.path / (name + " 2") / "folder/message.txt") == "archive payload\n");
    CHECK(fs::exists(archive));
}

TEST_CASE("archives: gzip keeps its source and uses the selected destination") {
    Fixture f;
    if (!f.install("gzip"))
        return;
    const auto archive = f.archive("message.txt.gz", "message.txt.gz");
    ActionHandoff::DirectoryChosen choose;
    ActionHandoff handoff{
        f.loop, f.environment, f.report(), {}, [&](const fs::path& initial, auto done) {
            CHECK(initial == f.dir.path);
            choose = std::move(done);
        }};
    ActionContext context{.directory = f.dir.path, .targets = {{archive, "application/gzip"}}};
    handoff.run(rummage::core::action_registry()[2], context);
    REQUIRE(choose);
    context.targets.clear();  // changing tabs/selection does not redirect a pending extraction
    choose(f.destination);
    REQUIRE(f.wait());
    CHECK(f.errors.empty());
    CHECK(read(f.destination / "message.txt") == "archive payload\n");
    CHECK(fs::exists(archive));
    CHECK_FALSE(fs::exists(f.dir.path / "message.txt"));
    CHECK_FALSE(fs::exists(f.destination / "message.txt (extracting)"));
}

TEST_CASE("archives: destination cancellation creates nothing") {
    Fixture f;
    if (!f.install("gzip"))
        return;
    const auto archive = f.archive("message.txt.gz", "message.txt.gz");
    ActionHandoff handoff{
        f.loop, f.environment, f.report(), {}, [](const auto&, auto done) { done(std::nullopt); }};
    handoff.run(rummage::core::action_registry()[2],
                {.directory = f.dir.path, .targets = {{archive, "application/gzip"}}});
    CHECK(f.messages.empty());
    CHECK(f.errors.empty());
    CHECK(fs::is_empty(f.destination));
    CHECK_FALSE(fs::exists(f.dir.path / "message.txt (extracting)"));
}

TEST_CASE("archives: tar unpacks both gzip and tar layers") {
    Fixture f;
    if (!f.install("tar") || !f.install("gzip"))
        return;
    const auto archive = f.archive("sample.tar.gz", "sample.tar.gz");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.dir.path, .targets = {{archive, "application/x-compressed-tar"}}});
    REQUIRE(f.wait());
    for (const auto& error : f.errors)
        INFO(error);
    CHECK(f.errors.empty());
    CHECK(read(f.dir.path / "sample/folder/message.txt") == "archive payload\n");
    CHECK(fs::exists(archive));
}

TEST_CASE("archives: 7z extracts 7z and both layers of compressed tar without tar installed") {
    Fixture f;
    if (!f.install("7z"))
        return;
    const auto seven = f.archive("sample.7z", "seven.7z");
    const auto tar = f.archive("sample.tar.gz", "tarball.tar.gz");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.dir.path,
                 .targets = {{seven, "application/x-7z-compressed"},
                             {tar, "application/x-compressed-tar"}}});
    REQUIRE(f.wait(2));
    for (const auto& error : f.errors)
        INFO(error);
    CHECK(f.errors.empty());
    CHECK(read(f.dir.path / "seven/folder/message.txt") == "archive payload\n");
    CHECK(read(f.dir.path / "tarball/folder/message.txt") == "archive payload\n");
    CHECK_FALSE(fs::exists(f.dir.path / "tarball (extracting)"));
}

TEST_CASE("archives: corrupt and encrypted archives report errors without publishing success") {
    Fixture f;
    if (!f.install("gzip") || !f.install("7z"))
        return;
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    const auto corrupt = f.dir.write("broken.gz", "not a gzip file");
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.dir.path, .targets = {{corrupt, "application/gzip"}}});
    REQUIRE(f.wait());
    REQUIRE_FALSE(f.errors.empty());
    CHECK_FALSE(fs::exists(f.dir.path / "broken"));
    CHECK(fs::exists(corrupt));
    f.errors.clear();
    const auto encrypted = f.archive("encrypted.7z", "secret.7z");
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.dir.path, .targets = {{encrypted, "application/x-7z-compressed"}}});
    REQUIRE(f.wait());
    CHECK_FALSE(f.errors.empty());
    CHECK_FALSE(fs::exists(f.dir.path / "secret"));
    CHECK(fs::exists(encrypted));
}

TEST_CASE("archives: File Roller remains a supported handoff with literal selected paths") {
    Fixture f;
    const auto shell = rummage::platform::find_program(
        "sh", rummage::platform::Environment::from_environment().path);
    REQUIRE(shell);
    const auto program = f.bin / "file-roller";
    std::ofstream{program} << "#!" << shell->string()
                           << "\nprintf '%s\\0' \"$@\" > called\nexit 7\n";
    fs::permissions(program, fs::perms::owner_all);
    const auto zip = f.archive("sample.zip", "a*[x] space.zip");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.run(rummage::core::action_registry()[2],
                {.directory = f.dir.path, .targets = {{zip, "application/zip"}}});
    REQUIRE(f.wait());
    CHECK(read(f.dir.path / "called") == std::string{"--extract"} + '\0' + zip.string() + '\0');
    CHECK_FALSE(fs::exists(f.dir.path / "a*[x] space (extracting)"));
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors.front().find("Exit status: 7") != std::string::npos);
}

TEST_CASE("archives: unsupported mixed selections do not start partial batches") {
    Fixture f;
    if (!f.install("gzip"))
        return;
    const auto gzip = f.archive("message.txt.gz", "message.txt.gz");
    const auto zip = f.archive("sample.zip", "sample.zip");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    const ActionContext context{.directory = f.dir.path,
                                .targets = {{gzip, "application/gzip"}, {zip, "application/zip"}}};
    CHECK_FALSE(handoff.available(rummage::core::action_registry()[1], context));
    handoff.run(rummage::core::action_registry()[1], context);
    CHECK(f.errors.size() == 1);
    CHECK(f.messages.empty());
    CHECK_FALSE(fs::exists(f.dir.path / "message.txt (extracting)"));
}

TEST_CASE("archives: canonical bzip2 tar MIME unpacks both layers with tar or 7z") {
    Fixture f;
    SUBCASE("tar") {
        if (!f.install("tar") || !f.install("bzip2"))
            return;
    }
    SUBCASE("7z") {
        if (!f.install("7z"))
            return;
    }
    // No extension: backend classification must use the canonical MIME too.
    const auto archive = f.archive("sample.tar.bz2", "bzip-tar");
    const auto backend = rummage::platform::archive_backend(
        {archive, "application/x-bzip2-compressed-tar"}, f.environment);
    REQUIRE(backend);
    CHECK((backend->kind == ArchiveBackend::Kind::tar || backend->tar_layer));
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.run(
        rummage::core::action_registry()[1],
        {.directory = f.dir.path, .targets = {{archive, "application/x-bzip2-compressed-tar"}}});
    REQUIRE(f.wait());
    CHECK(f.errors.empty());
    CHECK(read(f.dir.path / "bzip-tar 2/folder/message.txt") == "archive payload\n");
}

TEST_CASE("archives: ZIP RAR and 7z prefer password-capable CLI backends") {
    for (const auto* mime :
         {"application/zip", "application/x-7z-compressed", "application/vnd.rar",
          "application/x-rar", "application/x-rar-compressed"}) {
        Fixture f;
        const auto fake = f.dir.write("fake", "program");
        fs::permissions(fake, fs::perms::owner_all);
        const rummage::core::ActionTarget target{f.dir.path / "archive", mime};
        for (const auto* program : {"file-roller", "7z", "7zz"}) {
            fs::create_symlink(fake, f.bin / program);
            const auto backend = rummage::platform::archive_backend(target, f.environment);
            REQUIRE(backend);
            CHECK(backend->program.filename() == program);
        }
        fs::create_symlink(fake, f.bin / "unrar");
        const bool rar = std::string_view{mime}.find("rar") != std::string_view::npos;
        CHECK(rummage::platform::archive_backend(target, f.environment)->program.filename() ==
              (rar ? "unrar" : "7zz"));
    }
}

TEST_CASE("archives: encrypted ZIP RAR and 7z prompt and retry without File Roller") {
    for (const auto* fixture :
         {"encrypted-headers.7z", "encrypted-content.7z", "encrypted-unicode.7z",
          "encrypted-zipcrypto.zip", "encrypted-aes.zip", "encrypted-content.rar",
          "encrypted-headers.rar", "encrypted-content-rar5.rar", "encrypted-headers-rar5.rar"}) {
        for (const bool with_gui : {false, true}) {
            Fixture f;
            const auto extension = fs::path{fixture}.extension();
            const bool rar = extension == ".rar";
            if (!f.install(rar ? "unrar" : "7z"))
                continue;
            if (with_gui) {
                // This must not be invoked, even when present on PATH.
                const auto roller = f.dir.write("file-roller", "not an executable format");
                fs::permissions(roller, fs::perms::owner_all);
                fs::create_symlink(roller, f.bin / "file-roller");
            }
            CAPTURE(std::string{fixture});
            CAPTURE(with_gui);
            const auto archive = f.archive(fixture, "secret*[1] 'quoted'" + extension.string());
            unsigned prompts = 0;
            ActionHandoff handoff{f.loop, f.environment, f.report()};
            handoff.set_password_prompt([&](const auto& path, bool retry, auto done) {
                CHECK(path == archive);
                CHECK(retry == (prompts != 0));
                CHECK_FALSE(fs::exists(f.dir.path / "secret*[1] 'quoted' (extracting)"));
                ++prompts;
                REQUIRE(prompts <= 2);
                done(prompts == 1 ? "wrong"
                                  : (rar ? "password"
                                         : (std::string_view{fixture} == "encrypted-unicode.7z"
                                                ? "päss 🔒 archive test"
                                                : "archive test")));
            });
            const auto mime =
                rar ? "application/vnd.rar"
                    : (extension == ".zip" ? "application/zip" : "application/x-7z-compressed");
            handoff.run(rummage::core::action_registry()[1],
                        {.directory = f.dir.path, .targets = {{archive, mime}}});
            REQUIRE(f.wait());
            for (const auto& error : f.errors)
                INFO(error);
            CHECK(f.errors.empty());
            CHECK(prompts == 2);
            const auto output = f.dir.path / "secret*[1] 'quoted'";
            if (rar) {
                for (const auto* name : {"a.txt", "b.txt", "c.txt", "d.txt"})
                    CHECK(read(output / name) == "This is from " + std::string{name});
            } else {
                CHECK(read(output / "message.txt") == "archive payload\n");
            }
            CHECK(fs::exists(archive));
            CHECK_FALSE(fs::exists(f.dir.path / "secret*[1] 'quoted' (extracting)"));
        }
    }
}

TEST_CASE("archives: password cancellation and concurrent prompts are isolated") {
    Fixture f;
    if (!f.install("7z"))
        return;
    const auto first = f.archive("encrypted-headers.7z", "first.7z");
    const auto second = f.archive("encrypted-content.7z", "second.7z");
    std::vector<ActionHandoff::PasswordChosen> prompts;
    std::vector<fs::path> paths;
    ActionHandoff handoff{f.loop, f.environment, f.report(), {}, [&](const auto&, auto chosen) {
                              chosen(f.destination);
                          }};
    handoff.set_password_prompt([&](const auto& path, bool retry, auto done) {
        CHECK_FALSE(retry);
        paths.push_back(path);
        prompts.push_back(std::move(done));
    });
    handoff.run(rummage::core::action_registry()[2],
                {.directory = f.dir.path,
                 .targets = {{first, "application/x-7z-compressed"},
                             {second, "application/x-7z-compressed"}}});
    REQUIRE(f.until([&] { return !prompts.empty(); }));
    REQUIRE(prompts.size() == 1);
    auto cancel = std::move(prompts[0]);
    cancel(std::nullopt);
    REQUIRE(f.until([&] { return prompts.size() == 2; }));
    auto accept = std::move(prompts[1]);
    accept("archive test");
    REQUIRE(f.wait());
    CHECK(f.errors.empty());
    CHECK_FALSE(fs::exists(f.destination / paths[0].stem()));
    CHECK(read(f.destination / paths[1].stem() / "message.txt") == "archive payload\n");
}

TEST_CASE("archives: cancelling ZIP RAR or 7z passwords creates no output") {
    for (const auto* fixture :
         {"encrypted-content.7z", "encrypted-aes.zip", "encrypted-headers-rar5.rar"}) {
        Fixture f;
        const bool rar = fs::path{fixture}.extension() == ".rar";
        if (!f.install(rar ? "unrar" : "7z"))
            continue;
        const auto archive = f.archive(fixture, fixture);
        ActionHandoff handoff{f.loop, f.environment, f.report(), {}, [&](const auto&, auto chosen) {
                                  chosen(f.destination);
                              }};
        handoff.set_password_prompt([](const auto&, bool, auto done) { done(std::nullopt); });
        const auto mime =
            rar ? "application/vnd.rar"
                : (fs::path{fixture}.extension() == ".zip" ? "application/zip"
                                                           : "application/x-7z-compressed");
        handoff.run(rummage::core::action_registry()[2],
                    {.directory = f.dir.path, .targets = {{archive, mime}}});
        REQUIRE(f.until([&] { return !f.messages.empty() || !f.errors.empty(); }));
        CHECK(f.errors.empty());
        REQUIRE(f.messages.size() == 1);
        CHECK(f.messages[0].starts_with("Extraction cancelled:"));
        CHECK(fs::is_empty(f.destination));
        CHECK(fs::exists(archive));
    }
}

TEST_CASE("archives: a ZIP RAR and 7z batch extracts without a GUI backend") {
    Fixture f;
    if (!f.install("7z") || !f.install("unrar"))
        return;
    const auto zip = f.archive("encrypted-aes.zip", "zip.zip");
    const auto rar = f.archive("encrypted-headers-rar5.rar", "rar.rar");
    const auto seven = f.archive("encrypted-content.7z", "seven.7z");
    unsigned prompts = 0;
    ActionHandoff handoff{f.loop, f.environment, f.report(), {}, [&](const auto&, auto chosen) {
                              chosen(f.destination);
                          }};
    handoff.set_password_prompt([&](const auto& path, bool retry, auto done) {
        CHECK_FALSE(retry);
        ++prompts;
        done(path.extension() == ".rar" ? "password" : "archive test");
    });
    handoff.run(rummage::core::action_registry()[2],
                {.directory = f.dir.path,
                 .targets = {{zip, "application/zip"},
                             {rar, "application/vnd.rar"},
                             {seven, "application/x-7z-compressed"}}});
    REQUIRE(f.wait(3));
    CHECK(f.errors.empty());
    CHECK(prompts == 3);
    CHECK(read(f.destination / "zip/message.txt") == "archive payload\n");
    CHECK(read(f.destination / "rar/a.txt") == "This is from a.txt");
    CHECK(read(f.destination / "seven/message.txt") == "archive payload\n");
}

TEST_CASE("archives: a mixed 7z and tar selection keeps each chosen backend") {
    Fixture f;
    if (!f.install("7z"))
        return;
    const auto shell = rummage::platform::find_program(
        "sh", rummage::platform::Environment::from_environment().path);
    REQUIRE(shell);
    const auto program = f.bin / "file-roller";
    std::ofstream{program} << "#!" << shell->string() << "\nprintf '%s\\0' \"$@\" > called\n";
    fs::permissions(program, fs::perms::owner_all);
    const auto tar = f.archive("sample.tar.gz", "sample.tar.gz");
    const auto seven = f.archive("encrypted-content.7z", "seven.7z");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.set_password_prompt([](const auto&, bool, auto done) { done("archive test"); });
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.dir.path,
                 .targets = {{seven, "application/x-7z-compressed"},
                             {tar, "application/x-compressed-tar"}}});
    REQUIRE(f.wait());
    CHECK(f.errors.empty());
    CHECK(read(f.dir.path / "called") ==
          std::string{"--extract-here"} + '\0' + tar.string() + '\0');
    CHECK(read(f.dir.path / "seven/message.txt") == "archive payload\n");
}

TEST_CASE("archives: missing backends explain how to install a suitable extractor") {
    for (const auto* mime : {"application/vnd.rar", "application/zip",
                             "application/x-7z-compressed", "application/x-bzip2-compressed-tar"}) {
        Fixture f;
        ActionHandoff handoff{f.loop, f.environment, f.report()};
        handoff.run(rummage::core::action_registry()[1],
                    {.directory = f.destination, .targets = {{f.dir.path / "archive", mime}}});
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0].find("Install ") != std::string::npos);
        CHECK(f.errors[0].find("Archive: " + (f.dir.path / "archive").string()) !=
              std::string::npos);
        if (std::string_view{mime} == "application/vnd.rar")
            CHECK(f.errors[0].find("Install unrar and try again") != std::string::npos);
        CHECK(fs::is_empty(f.destination));
    }
}

TEST_CASE("archives: failure reports distinguish causes and preserve useful context") {
    const rummage::core::ActionTarget target{"/source/archive.rar", "application/vnd.rar"};
    const ArchiveBackend backend{.kind = ArchiveBackend::Kind::seven_zip, .program = "/bin/7zz"};
    for (const auto& [diagnostic, guidance] : std::vector<std::pair<std::string, std::string>>{
             {"ERROR: Unsupported Method", "Install unrar"},
             {"ERROR: Cannot open the file as archive", "Install unrar"},
             {"ERROR: No space left on device", "Free space"},
             {"ERROR: Permission denied", "writable destination"},
             {"ERROR: Unexpected end of archive", "fresh copy"},
             {"ERROR: CRC Failed", "damaged"},
             {"ERROR: Cannot find volume part2.rar", "all parts"},
             {"ERROR: Wrong password", "password was rejected"},
             {"", "did not provide diagnostic output"}}) {
        CAPTURE(diagnostic);
        const auto message =
            rummage::platform::archive_failure(target, backend, "extract", 2, 0, diagnostic,
                                               "/output", "/output/archive (extracting)");
        CHECK(message.find(guidance) != std::string::npos);
        CHECK(message.find("Archive: /source/archive.rar") != std::string::npos);
        CHECK(message.find("Extractor: /bin/7zz") != std::string::npos);
        CHECK(message.find("Exit status: 2; signal: 0") != std::string::npos);
        CHECK(message.find("Destination: /output") != std::string::npos);
        CHECK(message.find("Partial output (not published): /output/archive (extracting)") !=
              std::string::npos);
    }
    const auto interrupted =
        rummage::platform::archive_failure(target, backend, "inspect", 0, 15, "");
    CHECK(interrupted.find("stopped by a signal") != std::string::npos);
    CHECK(interrupted.find("signal: 15") != std::string::npos);
    CHECK(interrupted.find("No extraction output was created") != std::string::npos);
    CHECK_FALSE(rummage::platform::archive_password_error(
        backend, 2, "Unsupported encryption. Wrong password?"));
    const auto redacted = rummage::platform::archive_failure(
        target, backend, "decrypt", 2, 0,
        "Wrong password: private-secret\nRepeated: private-secret\033", {}, {}, "private-secret");
    CHECK(redacted.find("private-secret") == std::string::npos);
    CHECK(redacted.find("[redacted]") != std::string::npos);
    CHECK(redacted.find('\033') == std::string::npos);
}

TEST_CASE("archives: a RAR-incapable 7z gives installation guidance without a password loop") {
    Fixture f;
    f.fake("7z", "printf '%s\\n' 'Cannot open the file as archive' >&2\nexit 2\n");
    const auto archive = f.archive("encrypted-content.rar", "unsupported.rar");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.set_password_prompt(
        [](const auto&, bool, auto) { FAIL("must not request a password for a missing decoder"); });
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.destination, .targets = {{archive, "application/vnd.rar"}}});
    REQUIRE(f.wait());
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].find("Install unrar") != std::string::npos);
    CHECK(f.errors[0].find("Exit status: 2") != std::string::npos);
    CHECK(f.errors[0].find("Cannot open the file as archive") != std::string::npos);
    CHECK(fs::is_empty(f.destination));
}

TEST_CASE("archives: decryption failures retain diagnostics and redact the submitted password") {
    Fixture f;
    f.fake("7z", R"(case "$1" in
l) printf '%s\n' 'Encrypted = +'; exit 0;;
t) read -r secret; printf 'Unsupported encryption for password %s\n' "$secret" >&2; exit 2;;
esac
exit 7
)");
    const auto archive = f.archive("encrypted-content.7z", "secret.7z");
    unsigned prompts = 0;
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.set_password_prompt([&](const auto&, bool, auto done) {
        ++prompts;
        REQUIRE(prompts == 1);
        done("private-secret");
    });
    handoff.run(
        rummage::core::action_registry()[1],
        {.directory = f.destination, .targets = {{archive, "application/x-7z-compressed"}}});
    REQUIRE(f.wait());
    REQUIRE(f.errors.size() == 1);
    CHECK(prompts == 1);
    CHECK(f.errors[0].find("verify the password and archive") != std::string::npos);
    CHECK(f.errors[0].find("Install 7zz") != std::string::npos);
    CHECK(f.errors[0].find("private-secret") == std::string::npos);
    CHECK(f.errors[0].find("[redacted]") != std::string::npos);
    CHECK(fs::is_empty(f.destination));
}

TEST_CASE("archives: long diagnostics retain the final error and output location") {
    Fixture f;
    f.fake("gzip",
           "printf '" + std::string(10000, 'x') + "\\nNo space left on device\\n' >&2\nexit 1\n");
    const auto archive = f.archive("message.txt.gz", "message.txt.gz");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.destination, .targets = {{archive, "application/gzip"}}});
    REQUIRE(f.wait());
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].find("Free space") != std::string::npos);
    CHECK(f.errors[0].find("truncated; final lines follow") != std::string::npos);
    CHECK(f.errors[0].find("Partial output (not published)") != std::string::npos);
    CHECK(f.errors[0].size() < 10000);
    CHECK_FALSE(fs::exists(f.destination / "message.txt"));
}

TEST_CASE("archives: silent and interrupted extractors still provide actionable errors") {
    for (const auto* body : {"exit 9\n", "kill -TERM $$\n"}) {
        Fixture f;
        f.fake("7z", body);
        const auto archive = f.archive("sample.7z", "sample.7z");
        ActionHandoff handoff{f.loop, f.environment, f.report()};
        handoff.run(
            rummage::core::action_registry()[1],
            {.directory = f.destination, .targets = {{archive, "application/x-7z-compressed"}}});
        REQUIRE(f.wait());
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0].find(archive.string()) != std::string::npos);
        CHECK(f.errors[0].find("did not provide diagnostic output") != std::string::npos);
        CHECK(f.errors[0].find(std::string_view{body}.starts_with("exit")
                                   ? "Exit status: 9"
                                   : "signal: 15") != std::string::npos);
        CHECK(fs::is_empty(f.destination));
    }
}

TEST_CASE("archives: missing source and invalid destination retain filesystem diagnostics") {
    Fixture f;
    if (!f.install("gzip"))
        return;
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    const auto missing = f.dir.path / "missing.gz";
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.destination, .targets = {{missing, "application/gzip"}}});
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].find(missing.string()) != std::string::npos);
    CHECK(f.errors[0].find("No such file") != std::string::npos);
    CHECK(f.errors[0].find("readable") != std::string::npos);
    f.errors.clear();
    const auto file = f.dir.write("not-a-directory", "existing");
    const auto archive = f.archive("message.txt.gz", "message.txt.gz");
    handoff.run(rummage::core::action_registry()[1],
                {.directory = file, .targets = {{archive, "application/gzip"}}});
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].find(file.string()) != std::string::npos);
    CHECK(f.errors[0].find("writable destination") != std::string::npos);
    CHECK(read(file) == "existing");
}

TEST_CASE("archives: encrypted ZIP with only unzip explains the password-capable alternative") {
    Fixture f;
    if (!f.install("unzip"))
        return;
    const auto archive = f.archive("encrypted-zipcrypto.zip", "encrypted.zip");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.destination, .targets = {{archive, "application/zip"}}});
    REQUIRE(f.wait());
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].find("Install 7zz") != std::string::npos);
    CHECK(f.errors[0].find("password") != std::string::npos);
}

TEST_CASE("archives: File Roller failures retain its diagnostic and RAR installation guidance") {
    Fixture f;
    f.fake("file-roller",
           "printf '%s\\n' 'libarchive: RAR encryption support unavailable' >&2\nexit 2\n");
    const auto archive = f.archive("encrypted-headers.rar", "secret.rar");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.run(rummage::core::action_registry()[1],
                {.directory = f.destination, .targets = {{archive, "application/vnd.rar"}}});
    REQUIRE(f.wait());
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].find("Install unrar") != std::string::npos);
    CHECK(f.errors[0].find("libarchive: RAR encryption support unavailable") != std::string::npos);
    CHECK(f.errors[0].find("Exit status: 2") != std::string::npos);
    CHECK(f.errors[0].find("Check the destination chosen in File Roller for partial output") !=
          std::string::npos);
    CHECK(f.errors[0].find("No extraction output was created") == std::string::npos);
}

TEST_CASE("archives: an extractor that cannot start reports its path and system error") {
    Fixture f;
    const auto program = f.bin / "7z";
    std::ofstream{program} << "#!/rummage-test-nonexistent-interpreter\n";
    fs::permissions(program, fs::perms::owner_all);
    const auto archive = f.archive("sample.7z", "sample.7z");
    ActionHandoff handoff{f.loop, f.environment, f.report()};
    handoff.run(
        rummage::core::action_registry()[1],
        {.directory = f.destination, .targets = {{archive, "application/x-7z-compressed"}}});
    REQUIRE(f.errors.size() == 1);
    CHECK(f.errors[0].find("Cannot start " + program.string()) != std::string::npos);
    CHECK(f.errors[0].find("No such file") != std::string::npos);
    CHECK(f.errors[0].find("Install 7zz") != std::string::npos);
    CHECK(fs::is_empty(f.destination));
}
