#include "nordend/platform/action_handoff.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <format>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "nordend/core/i18n.hpp"
#include "nordend/fs/operations.hpp"

namespace nordend::platform {
namespace {

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
                           const std::filesystem::path& destination) {
    auto args = core::action_arguments(action, context, destination);
    if (args.empty()) {
        return;
    }
    const auto program = find_program(args.front(), environment_.path);
    if (!program) {
        report_(std::format("{} is not installed or not executable", args.front()), true);
        return;
    }
    args.front() = std::filesystem::absolute(*program).string();
    const auto start = [this, alive = alive_, args = std::move(args), directory = context.directory,
                        label = action.label,
                        notify = action.notify_on_completion](std::string token) {
        if (!*alive) {
            return;
        }
        std::vector<std::string> env;
        if (!token.empty()) {
            env.push_back("XDG_ACTIVATION_TOKEN=" + token);
        }
        const auto error = launcher_.spawn(
            args, directory, env, [this, alive, label, notify](const Launcher::Exit& result) {
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
                if (failed || notify) {
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
            });
        report_(error ? i18n::trf("Could not start {}: {}", label, error.message())
                      : i18n::trf("Handed off: {}", label),
                static_cast<bool>(error));
    };
    if (activate_) {
        activate_(start);
    } else {
        start({});
    }
}

}  // namespace nordend::platform

namespace nordend::platform {
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
    return std::make_shared<Descriptor>(memfd_create("nordend-action-output", MFD_CLOEXEC));
}

std::string diagnostic(int fd) {
    std::array<char, 512> bytes{};
    const auto count = pread(fd, bytes.data(), bytes.size(), 0);
    if (count <= 0)
        return {};
    std::string text{bytes.data(), static_cast<std::size_t>(count)};
    for (auto& c : text) {
        if (static_cast<unsigned char>(c) < 32)
            c = ' ';
    }
    return text;
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
    SharedFd input;
    SharedFd output;
    SharedFd errors;
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
    if (!available(action, context)) {
        report_(std::string{i18n::tr("No installed extractor supports every selected archive")},
                true);
        return;
    }
    // A GUI archive manager owns its chooser, passwords and conflict dialogs.
    if (available("file-roller")) {
        auto external = action;
        external.kind = core::Action::Kind::external;
        external.argv = {
            "file-roller",
            action.kind == core::Action::Kind::extract_here ? "--extract-here" : "--extract",
            "{files}"};
        run(external, context);
        return;
    }
    const auto start = [this, alive = alive_,
                        context](std::optional<std::filesystem::path> destination) {
        if (!*alive || !destination)
            return;
        // Resolve the whole selection again after the chooser, before starting any extraction.
        std::vector<ArchiveBackend> backends;
        for (const auto& target : context.targets) {
            const auto backend = archive_backend(target, environment_);
            if (!backend || backend->kind == ArchiveBackend::Kind::file_roller) {
                report_(
                    std::string{i18n::tr("Available archive programs changed; please try again")},
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
    std::error_code lookup_error;
    const auto program = std::filesystem::read_symlink("/proc/self/exe", lookup_error);
    if (output->fd < 3 || lookup_error) {
        report_(std::string{i18n::tr("Cannot create the destination picker")}, true);
        chosen(std::nullopt);
        return;
    }
    const auto start = [this, alive = alive_, output, program, request, chosen](std::string token) {
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
            [this, alive, output, chosen,
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
                report_(
                    std::string{i18n::tr("The destination picker could not select a valid path")},
                    true);
                chosen(std::nullopt);
            },
            output->fd);
        if (error) {
            report_(i18n::trf("Could not open destination picker: {}", error.message()), true);
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
    job->destination = destination;
    job->name = archive_output_name(target.path);
    job->input =
        std::make_shared<Descriptor>(open(target.path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    job->errors = channel();
    struct stat info{};
    if (job->input->fd < 3 || job->errors->fd < 3 || fstat(job->input->fd, &info) != 0 ||
        !S_ISREG(info.st_mode)) {
        report_(i18n::trf("Cannot read archive: {}", target.path.string()), true);
        return;
    }
    const auto made = fs::make_folder(destination, job->name + " (extracting)");
    if (made.error) {
        report_(i18n::trf("Cannot create extraction folder: {}", made.error.message()), true);
        return;
    }
    job->staging = destination / made.name;
    job->contents = job->staging / "contents";
    std::error_code error;
    std::filesystem::permissions(job->staging, std::filesystem::perms::owner_all, error);
    if (!error)
        std::filesystem::create_directory(job->contents, error);
    if (error) {
        report_(i18n::trf("Cannot prepare extraction: {}", error.message()), true);
        return;
    }
    report_(i18n::trf("Extracting {}…", target.path.filename().string()), false);
    extract_step(std::move(job));
}

void ActionHandoff::extract_step(std::shared_ptr<Extraction> job, bool second_pass) {
    const bool stream = !second_pass && (job->backend.tar_layer || job->backend.single_file);
    auto backend = job->backend;
    if (second_pass) {
        backend.tar_layer = false;
        backend.single_file = false;
        job->input = std::make_shared<Descriptor>(
            open((job->staging / "payload").c_str(), O_RDONLY | O_CLOEXEC));
    }
    job->output.reset();
    if (stream) {
        job->output = std::make_shared<Descriptor>(open(
            (job->staging / "payload").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    }
    if (job->input->fd < 3 || (stream && job->output->fd < 3)) {
        report_(i18n::trf("Cannot prepare archive output in {}", job->staging.string()), true);
        return;
    }
    // A regular inherited fd avoids filename wildcard expansion inside unzip and 7z, and
    // pins the source through both process launch and extraction. No shell or command string.
    const auto args = archive_command(backend, "/proc/self/fd/0", stream);
    const auto error = launcher_.spawn(
        args, job->contents,
        std::vector<std::string>{"UNZIP=", "UNZIPOPT=", "ZIPINFO=", "ZIPINFOOPT=", "TAR_OPTIONS=",
                                 "GZIP=", "PATH=" + environment_.path},
        [this, alive = alive_, job, second_pass](const Launcher::Exit& result) {
            if (!*alive)
                return;
            if (result.status != 0 || result.signal != 0) {
                report_(
                    i18n::trf("Could not extract {} (status {}, signal {}). Partial output: {}. {}",
                              job->source.filename().string(), result.status, result.signal,
                              job->staging.string(), diagnostic(job->errors->fd)),
                    true);
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
                report_(i18n::trf("Extraction finished but could not be renamed: {}. Output: {}",
                                  publish_error.message(), job->staging.string()),
                        true);
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
        stream ? job->output->fd : -1, job->errors->fd, job->input->fd);
    if (error) {
        report_(i18n::trf("Could not start extractor: {}. Output: {}", error.message(),
                          job->staging.string()),
                true);
    }
}

}  // namespace nordend::platform
