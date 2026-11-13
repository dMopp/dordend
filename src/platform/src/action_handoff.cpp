#include "rummage/platform/action_handoff.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <format>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/operations.hpp"

namespace rummage::platform {
namespace {

struct Descriptor {
    int fd{-1};

    explicit Descriptor(int value) : fd{value} {}

    ~Descriptor() {
        if (fd >= 0)
            close(fd);
    }

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};

using SharedFd = std::shared_ptr<Descriptor>;

SharedFd channel() {
    return std::make_shared<Descriptor>(memfd_create("rummage-action-output", MFD_CLOEXEC));
}

std::string diagnostic(int fd) {
    if (fd < 0)
        return {};
    constexpr std::size_t limit = 4096;
    std::array<char, limit> bytes{};
    const auto count = pread(fd, bytes.data(), bytes.size(), 0);
    if (count <= 0)
        return {};
    std::string text{bytes.data(), static_cast<std::size_t>(count)};
    struct stat info{};
    if (fstat(fd, &info) == 0 && info.st_size > static_cast<off_t>(limit)) {
        text += "\n[diagnostic output truncated; final lines follow]\n";
        const auto tail =
            pread(fd, bytes.data(), bytes.size(),
                  std::max(static_cast<off_t>(limit), info.st_size - static_cast<off_t>(limit)));
        if (tail > 0)
            text.append(bytes.data(), static_cast<std::size_t>(tail));
    }
    return text;
}

bool valid_destination(const std::filesystem::path& path, bool directory) {
    if (!path.is_absolute() || path.native().find('\0') != std::string::npos) {
        return false;
    }
    std::error_code error;
    if (directory) {
        return std::filesystem::is_directory(path, error);
    }
    if (path.filename().empty() || path.filename() == "." || path.filename() == ".." ||
        !std::filesystem::is_directory(path.parent_path(), error)) {
        return false;
    }
    const auto status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return false;
    }
    return !std::filesystem::exists(status) || std::filesystem::is_regular_file(path, error);
}

}  // namespace

ActionHandoff::ActionHandoff(core::EventLoop& loop, Environment environment, Report report,
                             Activate activate, ChooseDirectory choose,
                             ChooseDestination destination_picker, Notify notify)
    : launcher_{loop},
      environment_{std::move(environment)},
      report_{std::move(report)},
      activate_{std::move(activate)},
      choose_{std::move(choose)},
      destination_picker_{std::move(destination_picker)},
      notifications_{loop},
      notify_{std::move(notify)} {}

ActionHandoff::~ActionHandoff() { *alive_ = false; }

bool ActionHandoff::available(const std::string& program) const {
    return find_program(program, environment_.path).has_value();
}

void ActionHandoff::run(const core::Action& action, const core::ActionContext& context) {
    if (!core::action_matches(action, context))
        return;
    if (action.kind == core::Action::Kind::extract_here ||
        action.kind == core::Action::Kind::extract_to) {
        run_archive(action, context);
        return;
    }
    if (action.destination != core::Action::Destination::none) {
        if (action.argv.empty() || !available(action.argv.front())) {
            report_(std::string{i18n::tr("The action program is not installed or executable")},
                    true);
            return;
        }
        core::PickerRequest request;
        request.directory = action.destination == core::Action::Destination::directory;
        request.mode =
            request.directory ? core::PickerRequest::Mode::open : core::PickerRequest::Mode::save;
        request.folder = context.directory.string();
        request.name = action.destination_name;
        request.title = action.label;
        const auto chosen = [this, alive = alive_, action, context, directory = request.directory](
                                std::optional<std::filesystem::path> path) {
            if (!*alive || !path) {
                return;
            }
            if (!valid_destination(*path, directory)) {
                report_(std::string{i18n::tr("The selected destination is no longer valid")}, true);
                return;
            }
            launch(action, context, *path);
        };
        if (destination_picker_) {
            destination_picker_(request, chosen);
        } else {
            choose_path(request, chosen);
        }
        return;
    }
    launch(action, context);
}

void ActionHandoff::launch(const core::Action& action, const core::ActionContext& context,
                           const std::filesystem::path& destination, bool archive) {
    auto args = core::action_arguments(action, context, destination);
    if (args.empty()) {
        return;
    }
    const auto program = find_program(args.front(), environment_.path);
    if (!program) {
        if (archive) {
            for (const auto& target : context.targets)
                report_("Cannot start the archive manager: " + args.front() +
                            " is not installed or executable.\n" + archive_install_hint(target) +
                            "\nArchive: " + target.path.string(),
                        true);
        } else {
            report_(std::format("{} is not installed or not executable", args.front()), true);
        }
        return;
    }
    args.front() = std::filesystem::absolute(*program).string();
    auto errors = archive ? channel() : SharedFd{};
    if (errors && errors->fd < 3) {
        report_("Cannot prepare archive diagnostics: " +
                    std::error_code{errno, std::generic_category()}.message() +
                    ". Close unused applications and try again.",
                true);
        return;
    }
    const auto start = [this, alive = alive_, args = std::move(args), directory = context.directory,
                        label = action.label, context, errors, archive,
                        notify = action.notify_on_completion](std::string token) {
        if (!*alive) {
            return;
        }
        std::vector<std::string> env;
        if (!token.empty()) {
            env.push_back("XDG_ACTIVATION_TOKEN=" + token);
        }
        const auto error = launcher_.spawn(
            args, directory, env,
            [this, alive, label, notify, archive, errors, context,
             program = args.front()](const Launcher::Exit& result) {
                if (!*alive) {
                    return;
                }
                const bool failed = result.status != 0 || result.signal != 0;
                const auto message =
                    result.signal != 0
                        ? i18n::trf("Program stopped by signal {}", result.signal)
                        : (result.status != 0
                               ? i18n::trf("Program exited with status {}", result.status)
                               : std::string{i18n::tr("Completed successfully")});
                if (failed && archive) {
                    const ArchiveBackend backend{.kind = ArchiveBackend::Kind::file_roller,
                                                 .program = program};
                    for (const auto& target : context.targets)
                        report_(archive_failure(target, backend, "extract", result.status,
                                                result.signal, diagnostic(errors->fd)),
                                true);
                } else if (failed || notify) {
                    report_(std::format("{}: {}", label, message), failed);
                }
                if (notify) {
                    const auto title = std::format(
                        "{}: {}", failed ? i18n::tr("Failed") : i18n::tr("Completed"), label);
                    if (notify_) {
                        notify_(title, message, failed);
                    } else {
                        notifications_.send(title, message, failed);
                    }
                }
            },
            -1, errors ? errors->fd : -1);
        if (error && archive) {
            for (const auto& target : context.targets)
                report_("Cannot start " + args.front() + ": " + error.message() + "\n" +
                            archive_install_hint(target) + "\nArchive: " + target.path.string() +
                            "\nNo extraction was started.",
                        true);
        } else {
            report_(error ? std::format("Could not start {}: {}. Check that the program is "
                                        "installed and executable, then retry.",
                                        label, error.message())
                          : i18n::trf("Handed off: {}", label),
                    static_cast<bool>(error));
        }
    };
    if (activate_) {
        activate_(start);
    } else {
        start({});
    }
}

}  // namespace rummage::platform

namespace rummage::platform {
namespace {

bool contains_encrypted_entry(int fd, bool rar) {
    std::array<char, 4096> bytes{};
    std::string tail;
    off_t offset = 0;
    for (;;) {
        const auto size = pread(fd, bytes.data(), bytes.size(), offset);
        if (size <= 0)
            return false;
        tail.append(bytes.data(), static_cast<std::size_t>(size));
        std::size_t end = 0;
        while ((end = tail.find('\n')) != std::string::npos) {
            std::string_view line{tail.data(), end};
            const auto first = line.find_first_not_of(" \t");
            if (first != std::string_view::npos)
                line.remove_prefix(first);
            if ((!rar && line == "Encrypted = +") ||
                (rar && line.starts_with("Flags:") &&
                 line.find("encrypted") != std::string_view::npos))
                return true;
            tail.erase(0, end + 1);
        }
        offset += size;
        // Only short metadata lines matter; do not accumulate arbitrarily long filenames.
        if (tail.size() > bytes.size())
            tail.erase(0, tail.size() - bytes.size());
    }
}

std::filesystem::path publish(const std::filesystem::path& from,
                              const std::filesystem::path& directory, const std::string& name,
                              std::error_code& error) {
    for (unsigned i = 1; i < 10000; ++i) {
        const auto to = directory / (i == 1 ? name : std::format("{} {}", name, i));
        if (renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) {
            return to;
        }
        if (errno != EEXIST) {
            error = {errno, std::generic_category()};
            return {};
        }
    }
    error = std::make_error_code(std::errc::file_exists);
    return {};
}

}  // namespace

struct ActionHandoff::Extraction {
    ArchiveBackend backend;
    std::filesystem::path source;
    std::filesystem::path destination;
    std::filesystem::path staging;
    std::filesystem::path contents;
    std::string name;
    std::string mime;
    std::string password_text;
    SharedFd input;
    SharedFd output;
    SharedFd errors;
    SharedFd password;
};

bool ActionHandoff::available(const core::Action& action,
                              const core::ActionContext& context) const {
    if (action.kind == core::Action::Kind::external) {
        return !action.argv.empty() && available(action.argv.front());
    }
    if (action.kind == core::Action::Kind::copy_location)
        return true;
    return !context.targets.empty() &&
           std::ranges::all_of(context.targets, [&](const auto& target) {
               return archive_backend(target, environment_).has_value();
           });
}

void ActionHandoff::run_archive(const core::Action& action, const core::ActionContext& context) {
    bool missing = false;
    for (const auto& target : context.targets) {
        if (!archive_backend(target, environment_)) {
            report_("Cannot extract “" + target.path.filename().string() +
                        "”: no suitable extractor is installed.\n" + archive_install_hint(target) +
                        "\n\nArchive: " + target.path.string() +
                        "\nNo extraction in this selection was started.",
                    true);
            missing = true;
        }
    }
    if (missing)
        return;
    auto gui = context;
    auto cli = context;
    gui.targets.clear();
    cli.targets.clear();
    for (const auto& target : context.targets) {
        const auto backend = archive_backend(target, environment_);
        if (!backend) {
            report_("The archive extractor changed or is no longer available. " +
                        archive_install_hint(target) + "\nArchive: " + target.path.string(),
                    true);
            return;
        }
        (backend->kind == ArchiveBackend::Kind::file_roller ? gui : cli).targets.push_back(target);
    }
    // A GUI archive manager owns its chooser, passwords and conflict dialogs.
    if (!gui.targets.empty()) {
        auto external = action;
        external.kind = core::Action::Kind::external;
        external.argv = {
            "file-roller",
            action.kind == core::Action::Kind::extract_here ? "--extract-here" : "--extract",
            "{files}"};
        launch(external, gui, {}, true);
    }
    if (cli.targets.empty())
        return;
    const auto start = [this, alive = alive_,
                        context = cli](std::optional<std::filesystem::path> destination) {
        if (!*alive || !destination)
            return;
        // Resolve the whole selection again after the chooser, before starting any extraction.
        std::vector<ArchiveBackend> backends;
        for (const auto& target : context.targets) {
            const auto backend = archive_backend(target, environment_);
            if (!backend || backend->kind == ArchiveBackend::Kind::file_roller) {
                report_("The archive extractor changed or is no longer available. " +
                            archive_install_hint(target) + "\nArchive: " + target.path.string(),
                        true);
                return;
            }
            backends.push_back(*backend);
        }
        for (std::size_t i = 0; i < context.targets.size(); ++i) {
            extract(context.targets[i], backends[i], *destination);
        }
    };
    if (action.kind == core::Action::Kind::extract_to) {
        if (choose_)
            choose_(context.directory, start);
        else
            choose_destination(context.directory, start);
    } else {
        start(context.directory);
    }
}

void ActionHandoff::choose_destination(const std::filesystem::path& initial,
                                       DirectoryChosen chosen) {
    core::PickerRequest request;
    request.directory = true;
    request.folder = initial.string();
    request.title = std::string{i18n::tr("Extract To…")};
    choose_path(request, std::move(chosen));
}

void ActionHandoff::choose_path(const core::PickerRequest& request, DirectoryChosen chosen) {
    auto output = channel();
    auto errors = channel();
    std::error_code lookup_error;
    const auto program = std::filesystem::read_symlink("/proc/self/exe", lookup_error);
    if (output->fd < 3 || errors->fd < 3 || lookup_error) {
        const auto error =
            lookup_error ? lookup_error : std::error_code{errno, std::generic_category()};
        report_("Cannot open the destination picker: " + error.message() +
                    ". Reopen rummage and try again.\nInitial folder: " + request.folder,
                true);
        chosen(std::nullopt);
        return;
    }
    const auto start = [this, alive = alive_, output, errors, program, request,
                        chosen](std::string token) {
        if (!*alive)
            return;
        std::vector<std::string> env;
        if (!token.empty())
            env.push_back("XDG_ACTIVATION_TOKEN=" + token);
        std::vector<std::string> args{program.string(),
                                      request.directory ? "--pick-directory" : "--save-file",
                                      "--title", request.title};
        if (!request.directory && !request.name.empty()) {
            args.insert(args.end(), {"--name", request.name});
        }
        args.push_back(request.folder);
        const auto error = launcher_.spawn(
            args, request.folder, env,
            [this, alive, output, errors, chosen, request,
             directory = request.directory](const Launcher::Exit& result) {
                if (!*alive)
                    return;
                if (result.status == 1 && result.signal == 0) {
                    chosen(std::nullopt);
                    return;
                }
                std::array<char, PATH_MAX + 1> bytes{};
                const auto size = pread(output->fd, bytes.data(), bytes.size(), 0);
                if (result.status == 0 && result.signal == 0 && size > 1 &&
                    bytes[static_cast<std::size_t>(size) - 1] == '\0') {
                    const std::string path{bytes.data(), static_cast<std::size_t>(size) - 1};
                    if (valid_destination(path, directory)) {
                        chosen(std::filesystem::path{path});
                        return;
                    }
                }
                report_(std::format("The destination picker could not select a valid path. Choose "
                                    "an existing, accessible folder and try again.\nInitial "
                                    "folder: {}\nExit status: {}; signal: {}\n{}",
                                    request.folder, result.status, result.signal,
                                    diagnostic(errors->fd)),
                        true);
                chosen(std::nullopt);
            },
            output->fd, errors->fd);
        if (error) {
            report_("Could not open destination picker: " + error.message() +
                        ". Reopen rummage and try again.\nInitial folder: " + request.folder,
                    true);
            chosen(std::nullopt);
        }
    };
    if (activate_)
        activate_(start);
    else
        start({});
}

void ActionHandoff::extract(const core::ActionTarget& target, const ArchiveBackend& backend,
                            const std::filesystem::path& destination) {
    auto job = std::make_shared<Extraction>();
    job->backend = backend;
    job->source = target.path;
    job->mime = target.mime;
    job->destination = destination;
    job->name = archive_output_name(target.path);
    job->input =
        std::make_shared<Descriptor>(open(target.path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    if (job->input->fd < 3) {
        archive_problem(
            job,
            "Cannot read archive: " + std::error_code{errno, std::generic_category()}.message(),
            "Check that the source still exists and is readable, then try again.");
        return;
    }
    struct stat info{};
    if (fstat(job->input->fd, &info) != 0) {
        archive_problem(
            job,
            "Cannot inspect source: " + std::error_code{errno, std::generic_category()}.message(),
            "Check the source file and try again.");
        return;
    }
    if (!S_ISREG(info.st_mode)) {
        archive_problem(job, "The selected source is not a regular file.",
                        "Choose an archive file and try again.");
        return;
    }
    job->errors = channel();
    if (job->errors->fd < 3) {
        archive_problem(job,
                        "Cannot prepare diagnostics: " +
                            std::error_code{errno, std::generic_category()}.message(),
                        "Close unused applications and try again.");
        return;
    }
    if (backend.kind == ArchiveBackend::Kind::seven_zip ||
        backend.kind == ArchiveBackend::Kind::unrar)
        probe_archive(std::move(job));
    else
        extract_step(std::move(job));
}

void ActionHandoff::archive_problem(const std::shared_ptr<Extraction>& job, std::string reason,
                                    std::string guidance) {
    report_("Could not extract “" + job->source.filename().string() + "”.\n" + reason + "\n" +
                guidance + "\n\nArchive: " + job->source.string() +
                "\nDestination: " + job->destination.string() +
                (job->staging.empty() ? "\nNo extraction output was created."
                                      : "\nOutput retained at: " + job->staging.string()),
            true);
}

void ActionHandoff::fail_archive(const std::shared_ptr<Extraction>& job, std::string_view operation,
                                 const Launcher::Exit& result, int output_fd) {
    auto details = diagnostic(job->errors->fd);
    const auto output = diagnostic(output_fd);
    if (!output.empty())
        details += "\n" + output;
    report_(
        archive_failure({job->source, job->mime}, job->backend, operation, result.status,
                        result.signal, details, job->destination, job->staging, job->password_text),
        true);
}

// List first, so ordinary archives avoid a full verification pass. Encrypted archives are
// tested with the supplied password before creating output: retries cannot skip partial files.
void ActionHandoff::probe_archive(std::shared_ptr<Extraction> job, bool verify) {
    auto output = channel();
    if (output->fd < 3) {
        archive_problem(
            job,
            "Cannot inspect archive: " + std::error_code{errno, std::generic_category()}.message(),
            "Close unused applications and try again.");
        return;
    }
    auto backend = job->backend;
    backend.single_file = false;
    auto args =
        archive_command(backend, "/proc/self/fd/3", false, static_cast<bool>(job->password));
    const bool rar = backend.kind == ArchiveBackend::Kind::unrar;
    args[1] = verify ? "t" : (rar ? "lt" : "l");
    if (!verify) {
        if (rar) {
            std::erase(args, "-idq");
        } else {
            std::erase(args, "-bso0");
            args.insert(args.begin() + 2, "-slt");
        }
    }
    if (ftruncate(job->errors->fd, 0) != 0 || lseek(job->errors->fd, 0, SEEK_SET) < 0) {
        archive_problem(job,
                        "Cannot prepare diagnostics: " +
                            std::error_code{errno, std::generic_category()}.message(),
                        "Close unused applications and try again.");
        return;
    }
    if (job->password && lseek(job->password->fd, 0, SEEK_SET) < 0) {
        archive_problem(job,
                        "Cannot read the submitted password: " +
                            std::error_code{errno, std::generic_category()}.message(),
                        "Start extraction again and re-enter the password.");
        return;
    }
    const auto error = launcher_.spawn(
        args, {}, std::vector<std::string>{"LC_ALL=C.UTF-8", "RAR=", "PATH=" + environment_.path},
        [this, alive = alive_, job, output, verify, rar](const Launcher::Exit& result) {
            if (!*alive)
                return;
            const auto detail = diagnostic(job->errors->fd) + "\n" + diagnostic(output->fd);
            const bool wrong_password = archive_password_error(job->backend, result.status, detail);
            if (result.signal == 0 &&
                ((result.status != 0 && wrong_password) ||
                 (result.status == 0 && !verify && contains_encrypted_entry(output->fd, rar)))) {
                ask_password(job);
                return;
            }
            if (result.status != 0 || result.signal != 0) {
                fail_archive(job,
                             verify ? "verify the password and archive" : "inspect the archive",
                             result, output->fd);
                return;
            }
            extract_step(job);
        },
        output->fd, job->errors->fd, job->password ? job->password->fd : -1, job->input->fd);
    if (error)
        archive_problem(job,
                        "Cannot start " + job->backend.program.string() + ": " + error.message(),
                        archive_install_hint({job->source, job->mime}));
}

void ActionHandoff::ask_password(std::shared_ptr<Extraction> job) {
    if (!password_) {
        archive_problem(
            job, "This archive requires a password, but no password dialog is available.",
            "Reopen rummage and try again, or open the archive in another archive manager.");
        return;
    }
    passwords_.push_back(std::move(job));
    if (passwords_.size() == 1)
        next_password();
}

void ActionHandoff::next_password() {
    if (passwords_.empty())
        return;
    const auto job = passwords_.front();
    password_(
        job->source, static_cast<bool>(job->password),
        [this, alive = alive_, job](std::optional<std::string> password) {
            if (!*alive)
                return;
            passwords_.pop_front();
            if (password) {
                // Both tools read one UTF-8 password line from stdin, never argv or logs.
                if (password->find_first_of(std::string_view{"\r\n\0", 3}) != std::string::npos) {
                    archive_problem(job, "The password contains a line break or NUL character.",
                                    "Enter a single-line password and try again.");
                } else {
                    job->password_text = *password;
                    auto input = channel();
                    password->push_back('\n');
                    if (input->fd < 3 || write(input->fd, password->data(), password->size()) !=
                                             static_cast<ssize_t>(password->size())) {
                        archive_problem(
                            job,
                            "Cannot prepare the password: " +
                                std::error_code{errno, std::generic_category()}.message(),
                            "Close unused applications and start extraction again.");
                    } else {
                        job->password = std::move(input);
                        probe_archive(job, true);
                    }
                    std::fill(password->begin(), password->end(), '\0');
                }
            } else {
                report_("Extraction cancelled: " + job->source.filename().string(), false);
            }
            next_password();
        });
}

void ActionHandoff::extract_step(std::shared_ptr<Extraction> job, bool second_pass) {
    if (job->staging.empty()) {
        const auto made = fs::make_folder(job->destination, job->name + " (extracting)");
        if (made.error) {
            archive_problem(job, "Cannot create the extraction folder: " + made.error.message(),
                            "Choose a writable destination with enough free space.");
            return;
        }
        job->staging = job->destination / made.name;
        job->contents = job->staging / "contents";
        std::error_code error;
        std::filesystem::permissions(job->staging, std::filesystem::perms::owner_all, error);
        if (!error)
            std::filesystem::create_directory(job->contents, error);
        if (error) {
            archive_problem(job, "Cannot prepare the extraction folder: " + error.message(),
                            "Check destination permissions and free disk space, then retry.");
            return;
        }
        report_("Extracting " + job->source.filename().string() + "…", false);
    }
    const bool stream = !second_pass && (job->backend.tar_layer || job->backend.single_file);
    auto backend = job->backend;
    if (second_pass) {
        backend.tar_layer = false;
        backend.single_file = false;
        job->input = std::make_shared<Descriptor>(
            open((job->staging / "payload").c_str(), O_RDONLY | O_CLOEXEC));
        if (job->input->fd < 3) {
            archive_problem(job,
                            "Cannot read the unpacked tar data: " +
                                std::error_code{errno, std::generic_category()}.message(),
                            "Check the retained output folder and retry extraction.");
            return;
        }
    }
    job->output = stream ? SharedFd{} : channel();
    if (stream) {
        job->output = std::make_shared<Descriptor>(open(
            (job->staging / "payload").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    }
    if (job->input->fd < 3 || job->output->fd < 3) {
        archive_problem(job,
                        "Cannot prepare archive input or output: " +
                            std::error_code{errno, std::generic_category()}.message(),
                        "Check destination permissions and free disk space, then retry.");
        return;
    }
    // A regular inherited fd avoids filename wildcard expansion inside unzip and 7z, and
    // pins the source through both process launch and extraction. No shell or command string.
    const auto args =
        archive_command(backend, "/proc/self/fd/3", stream, static_cast<bool>(job->password));
    if (job->password && lseek(job->password->fd, 0, SEEK_SET) < 0) {
        archive_problem(job,
                        "Cannot read the submitted password: " +
                            std::error_code{errno, std::generic_category()}.message(),
                        "Start extraction again and re-enter the password.");
        return;
    }
    if (ftruncate(job->errors->fd, 0) != 0 || lseek(job->errors->fd, 0, SEEK_SET) < 0) {
        archive_problem(job,
                        "Cannot prepare diagnostics: " +
                            std::error_code{errno, std::generic_category()}.message(),
                        "Close unused applications and try again.");
        return;
    }
    const auto error = launcher_.spawn(
        args, job->contents,
        std::vector<std::string>{"UNZIP=", "UNZIPOPT=", "ZIPINFO=", "ZIPINFOOPT=", "TAR_OPTIONS=",
                                 "GZIP=", "LC_ALL=C.UTF-8", "RAR=", "PATH=" + environment_.path},
        [this, alive = alive_, job, second_pass, stream](const Launcher::Exit& result) {
            if (!*alive)
                return;
            if (result.status != 0 || result.signal != 0) {
                fail_archive(job, "extract", result, stream ? -1 : job->output->fd);
                return;
            }
            if (job->backend.tar_layer && !second_pass) {
                extract_step(job, true);
                return;
            }
            std::error_code publish_error;
            const auto output =
                publish(job->backend.single_file ? job->staging / "payload" : job->contents,
                        job->destination, job->name, publish_error);
            if (publish_error) {
                archive_problem(job,
                                "Extraction finished, but publishing the result failed: " +
                                    publish_error.message(),
                                "The extracted data is retained. Check destination permissions and "
                                "move it from the output folder below.");
                return;
            }
            // Only known temporary entries, never recursively remove archive-controlled paths.
            std::error_code ignored;
            if (job->backend.single_file)
                std::filesystem::remove(job->contents, ignored);
            if (job->backend.tar_layer)
                std::filesystem::remove(job->staging / "payload", ignored);
            std::filesystem::remove(job->staging, ignored);
            report_(i18n::trf("Extracted to {}", output.string()), false);
        },
        job->output->fd, job->errors->fd, job->password ? job->password->fd : -1, job->input->fd);
    if (error) {
        archive_problem(job,
                        "Cannot start " + job->backend.program.string() + ": " + error.message(),
                        archive_install_hint({job->source, job->mime}));
    }
}

}  // namespace rummage::platform
