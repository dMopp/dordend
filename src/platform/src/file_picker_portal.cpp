#include "nordend/platform/file_picker_portal.hpp"

#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <map>
#include <memory>
#include <poll.h>
#include <sdbus-c++/sdbus-c++.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "nordend/platform/bus_loop.hpp"
#include "nordend/core/file.hpp"
#include "nordend/core/file_picker.hpp"
#include "nordend/core/log.hpp"

extern char** environ;

namespace nordend::platform {
namespace {
using Values = std::map<std::string, sdbus::Variant>;
using Reply = sdbus::Result<std::uint32_t, Values>;
using Pattern = sdbus::Struct<std::uint32_t, std::string>;
using Filter = sdbus::Struct<std::string, std::vector<Pattern>>;
using Option = sdbus::Struct<std::string, std::string>;
using Choice = sdbus::Struct<std::string, std::string, std::vector<Option>, std::string>;
using Mode = core::PickerRequest::Mode;
constexpr auto kInterface = "org.freedesktop.impl.portal.FileChooser";

struct Fd {
    int value{-1};

    explicit Fd(int fd = -1) : value{fd} {}

    ~Fd() {
        if (value >= 0) {
            close(value);
        }
    }

    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};

template <class T>
T option(const Values& options, const char* name, T fallback = {}) {
    const auto it = options.find(name);
    return it == options.end() ? fallback : it->second.get<T>();
}

std::string path_option(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) {
        return {};
    }
    if (bytes.back() != 0 || std::find(bytes.begin(), bytes.end() - 1, 0) != bytes.end() - 1) {
        throw std::runtime_error{"A picker path must be NUL-terminated"};
    }
    return {bytes.begin(), bytes.end() - 1};
}

core::PickerFilter from_filter(const Filter& filter) {
    core::PickerFilter out;
    out.label = filter.get<0>();
    for (const auto& p : filter.get<1>()) {
        if (p.get<0>() > 1) {
            throw std::runtime_error{"Unknown file filter kind"};
        }
        out.patterns.push_back({p.get<0>(), p.get<1>()});
    }
    return out;
}

Filter to_filter(const core::PickerFilter& filter) {
    std::vector<Pattern> patterns;
    for (const auto& p : filter.patterns) {
        patterns.emplace_back(p.kind, p.value);
    }
    return {filter.label, std::move(patterns)};
}

core::PickerRequest request_from(Mode mode, const std::string& title, const std::string& parent,
                                 const Values& options) {
    core::PickerRequest r;
    r.mode = mode;
    r.title = title;
    r.parent_window = parent;
    r.multiple = mode == Mode::open && option<bool>(options, "multiple");
    r.directory = mode == Mode::open && option<bool>(options, "directory");
    r.modal = option<bool>(options, "modal", true);
    r.accept_label = option<std::string>(options, "accept_label");
    // Portal button labels use underscores for mnemonics; the picker uses explicit shortcuts.
    std::erase(r.accept_label, '_');
    r.folder = path_option(option<std::vector<std::uint8_t>>(options, "current_folder"));
    r.name = option<std::string>(options, "current_name");
    const auto current = path_option(option<std::vector<std::uint8_t>>(options, "current_file"));
    if (mode == Mode::save && !current.empty()) {
        const std::filesystem::path path{current};
        r.folder = path.parent_path().string();
        r.name = path.filename().string();
    }
    for (const auto& bytes : option<std::vector<std::vector<std::uint8_t>>>(options, "files")) {
        const auto name = path_option(bytes);
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos) {
            throw std::runtime_error{"Invalid SaveFiles filename"};
        }
        r.files.push_back(name);
    }
    if (mode == Mode::save_files && r.files.empty()) {
        throw std::runtime_error{"SaveFiles needs filenames"};
    }
    for (const auto& filter : option<std::vector<Filter>>(options, "filters")) {
        r.filters.push_back(from_filter(filter));
    }
    if (const auto it = options.find("current_filter"); it != options.end()) {
        const auto filter = from_filter(it->second.get<Filter>());
        const auto found = std::ranges::find(r.filters, filter);
        if (found == r.filters.end()) {
            r.filter = static_cast<std::uint32_t>(r.filters.size());
            r.filters.push_back(filter);
        } else {
            r.filter = static_cast<std::uint32_t>(found - r.filters.begin());
        }
    }
    for (const auto& c : option<std::vector<Choice>>(options, "choices")) {
        core::PickerChoice choice{c.get<0>(), c.get<1>(), {}, c.get<3>()};
        for (const auto& o : c.get<2>()) {
            choice.options.push_back({o.get<0>(), o.get<1>()});
        }
        r.choices.push_back(std::move(choice));
    }
    return r;
}

struct Pending {
    Reply reply;
    core::PickerRequest request;
    std::string sender;
    std::unique_ptr<sdbus::IObject> object;
    Fd output;
    Fd pidfd;
    core::EventLoop::SourceId source{};
    bool cancelled{};
    bool reaped{};

    explicit Pending(Reply result) : reply{std::move(result)} {}

    ~Pending() {
        if (pidfd.value >= 0 && !reaped) {
            syscall(SYS_pidfd_send_signal, pidfd.value, SIGKILL, nullptr, 0);
            siginfo_t info{};
            while (waitid(P_PIDFD, static_cast<id_t>(pidfd.value), &info, WEXITED) < 0 &&
                   errno == EINTR) {
            }
        }
    }
};

class Backend {
public:
    Backend(core::EventLoop& loop, std::filesystem::path picker_program)
        : loop_{loop},
          picker_program_{std::move(picker_program)},
          connection_{sdbus::createSessionBusConnection()},
          object_{sdbus::createObject(*connection_,
                                      sdbus::ObjectPath{"/org/freedesktop/portal/desktop"})} {
        const auto method = [this](Mode mode) {
            return [this, mode](Reply reply, const sdbus::ObjectPath& handle,
                                const std::string& /*app_id*/, const std::string& parent,
                                const std::string& title, const Values& options) {
                start(std::move(reply), handle, parent, title, options, mode);
            };
        };
        object_
            ->addVTable(
                sdbus::registerMethod("OpenFile").implementedAs(method(Mode::open)),
                sdbus::registerMethod("SaveFile").implementedAs(method(Mode::save)),
                sdbus::registerMethod("SaveFiles").implementedAs(method(Mode::save_files)),
                sdbus::registerProperty("version").withGetter([] { return std::uint32_t{4}; }))
            .forInterface(kInterface);
        names_ = connection_->addMatch(
            "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='"
            "NameOwnerChanged'",
            [this](sdbus::Message message) {
                std::string name, before, after;
                message >> name >> before >> after;
                if (after.empty()) {
                    for (const auto& [handle, pending] : pending_) {
                        if (pending->sender == name) {
                            cancel(*pending);
                        }
                    }
                }
            },
            sdbus::return_slot);
        connection_->requestName(sdbus::ServiceName{"org.freedesktop.impl.portal.desktop.nordend"});
        bus_ = std::make_unique<BusLoop>(loop_, *connection_, "file chooser");
        bus_->pump();
    }

    ~Backend() {
        for (const auto& [handle, pending] : pending_) {
            loop_.remove(pending->source);
            cancel(*pending);
        }
    }

private:
    static void cancel(Pending& pending) {
        pending.cancelled = true;
        // Kill only this picker. It performs no file writes for the requesting application.
        if (pending.pidfd.value >= 0) {
            syscall(SYS_pidfd_send_signal, pending.pidfd.value, SIGKILL, nullptr, 0);
        }
    }

    void start(Reply reply, const sdbus::ObjectPath& handle, const std::string& parent,
               const std::string& title, const Values& options, Mode mode) {
        auto pending = std::make_unique<Pending>(std::move(reply));
        try {
            if (pending_.size() >= 32 || pending_.contains(handle)) {
                throw std::runtime_error{"Too many picker requests or duplicate handle"};
            }
            pending->request = request_from(mode, title, parent, options);
            pending->sender = object_->getCurrentlyProcessedMessage().getSender();
            pending->object = sdbus::createObject(*connection_, handle);
            auto* ptr = pending.get();
            pending->object
                ->addVTable(sdbus::registerMethod("Close").implementedAs([ptr] {
                    if (ptr->object->getCurrentlyProcessedMessage().getSender() != ptr->sender) {
                        throw sdbus::Error{
                            sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"},
                            "Not your request"};
                    }
                    cancel(*ptr);
                }))
                .forInterface("org.freedesktop.impl.portal.Request");
            Fd input{memfd_create("nordend-picker-request", MFD_CLOEXEC)};
            pending->output.value = memfd_create("nordend-picker-result", MFD_CLOEXEC);
            if (input.value < 0 || pending->output.value < 0) {
                throw std::runtime_error{"Cannot allocate picker channel"};
            }
            core::write_picker_request(input.value, pending->request);
            // Move source descriptors above the inherited slots to avoid dup2 collisions.
            Fd in{fcntl(input.value, F_DUPFD_CLOEXEC, 5)};
            Fd out{fcntl(pending->output.value, F_DUPFD_CLOEXEC, 5)};
            if (in.value < 0 || out.value < 0) {
                throw std::runtime_error{"Cannot duplicate picker channel"};
            }
            posix_spawn_file_actions_t actions{};
            posix_spawnattr_t attr{};
            posix_spawn_file_actions_init(&actions);
            posix_spawnattr_init(&attr);
            posix_spawn_file_actions_adddup2(&actions, in.value, 3);
            posix_spawn_file_actions_adddup2(&actions, out.value, 4);
            posix_spawn_file_actions_addclosefrom_np(&actions, 5);
            sigset_t none{}, all{};
            sigemptyset(&none);
            sigfillset(&all);
            posix_spawnattr_setsigmask(&attr, &none);
            posix_spawnattr_setsigdefault(&attr, &all);
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
            std::string exe =
                (picker_program_.empty() ? std::filesystem::read_symlink("/proc/self/exe")
                                         : picker_program_)
                    .string();
            std::string flag = "--picker-channel";
            char* args[]{exe.data(), flag.data(), nullptr};
            const int error =
                pidfd_spawn(&pending->pidfd.value, exe.c_str(), &actions, &attr, args, environ);
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attr);
            if (error != 0) {
                throw std::runtime_error{"Cannot start Nordend picker: " +
                                         std::string{strerror(error)}};
            }
            pending->source =
                loop_.add_fd(pending->pidfd.value, POLLIN,
                             [this, key = std::string{handle}](short) { finish(key); });
            pending_.emplace(handle, std::move(pending));
        } catch (const std::exception& e) {
            log::error("file chooser: {}", e.what());
            if (pending) {
                pending->reply.returnResults(std::uint32_t{2}, Values{});
            }
        }
    }

    void finish(const std::string& key) {
        auto node = pending_.extract(key);
        if (node.empty()) {
            return;
        }
        auto& pending = *node.mapped();
        loop_.remove(pending.source);
        siginfo_t info{};
        int waited;
        do {
            waited = waitid(P_PIDFD, static_cast<id_t>(pending.pidfd.value), &info, WEXITED);
        } while (waited < 0 && errno == EINTR);
        pending.reaped = waited == 0 || errno == ECHILD;
        pending.object.reset();
        core::PickerResult result;
        result.response = pending.cancelled ? 1U : 2U;
        Values values;
        try {
            if (!pending.cancelled && waited == 0 && info.si_code == CLD_EXITED &&
                info.si_status == 0) {
                result = core::read_picker_result(pending.output.value);
                if (result.response == 0) {
                    const auto& request = pending.request;
                    if (result.paths.empty() ||
                        (request.mode == Mode::save && result.paths.size() != 1) ||
                        (request.mode == Mode::open && !request.multiple &&
                         result.paths.size() != 1) ||
                        (request.mode == Mode::save_files &&
                         result.paths.size() != request.files.size()) ||
                        result.choices.size() != request.choices.size()) {
                        throw std::runtime_error{"Invalid picker result"};
                    }
                    std::vector<std::string> uris;
                    for (const auto& path : result.paths) {
                        if (!std::filesystem::path{path}.is_absolute()) {
                            throw std::runtime_error{"Non-absolute picker result"};
                        }
                        uris.push_back(core::file_uri(path));
                    }
                    values.emplace("uris", sdbus::Variant{uris});
                    if (!request.filters.empty()) {
                        if (result.filter >= request.filters.size()) {
                            throw std::runtime_error{"Invalid result filter"};
                        }
                        values.emplace("current_filter",
                                       sdbus::Variant{to_filter(request.filters[result.filter])});
                    }
                    std::vector<Option> choices;
                    for (std::size_t i = 0; i < request.choices.size(); ++i) {
                        choices.emplace_back(request.choices[i].id, result.choices[i]);
                    }
                    if (!choices.empty()) {
                        values.emplace("choices", sdbus::Variant{choices});
                    }
                }
            }
        } catch (const std::exception& e) {
            log::error("file chooser: {}", e.what());
            result.response = 2;
            values.clear();
        }
        pending.reply.returnResults(result.response, values);
        bus_->pump();
    }

    core::EventLoop& loop_;
    std::filesystem::path picker_program_;
    std::unique_ptr<sdbus::IConnection> connection_;
    std::unique_ptr<sdbus::IObject> object_;
    sdbus::Slot names_;
    std::unique_ptr<BusLoop> bus_;
    std::map<std::string, std::unique_ptr<Pending>> pending_;
};
}  // namespace

int run_file_picker_portal(core::EventLoop& loop, const std::filesystem::path& picker_program) {
    Backend backend{loop, picker_program};
    return loop.run();
}
}  // namespace nordend::platform
