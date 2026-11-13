#include "rummage/isolate/helpers.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>
#include <span>
#include <spawn.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "rummage/core/log.hpp"
#include "rummage/core/worker_pool.hpp"
#include "protocol.hpp"

extern char** environ;  // NOLINT(readability-redundant-declaration): POSIX, not in a header

namespace rummage::isolate {
namespace {

picture::Loaded failed(picture::Failure failure, std::string detail) {
    picture::Loaded result;
    result.failure = failure;
    result.detail = std::move(detail);
    return result;
}

std::string errno_text() { return std::error_code{errno, std::generic_category()}.message(); }

// A regular file opened for reading, to be sent to a helper: the helper gets the descriptor,
// never the path. O_NONBLOCK, so that a FIFO named like a photo cannot hang the worker; -1
// (errno set) for anything that is not a regular file.
int open_regular(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        ::close(fd);
        errno = EINVAL;
        return -1;
    }
    return fd;
}

// The picture in a helper's memfd, checked against the size it said.
std::optional<picture::Image> read_pixels(int fd, std::uint32_t width, std::uint32_t height) {
    const std::uint64_t bytes = std::uint64_t{width} * height * 4;
    struct stat info{};
    if (width == 0 || height == 0 || ::fstat(fd, &info) != 0 ||
        std::cmp_not_equal(info.st_size, bytes)) {
        return std::nullopt;
    }
    // Sealed against shrinking: the mapping cannot lose pages under us.
    const int seals = ::fcntl(fd, F_GET_SEALS);
    if (seals < 0 || (seals & F_SEAL_SHRINK) == 0) {
        return std::nullopt;
    }
    void* mapped = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        return std::nullopt;
    }
    picture::Image picture{.width = width, .height = height, .rgba = {}};
    const auto* from = static_cast<const std::uint8_t*>(mapped);
    picture.rgba.assign(from, from + bytes);
    ::munmap(mapped, bytes);
    return picture;
}

// A helper's sealed memfd of exactly size bytes, read whole; nullopt when it is not that.
std::optional<std::vector<std::byte>> read_sealed(int fd, std::size_t size) {
    std::vector<std::byte> bytes(size);
    if (size == 0) {
        return bytes;
    }
    struct stat info{};
    if (fd < 0 || ::fstat(fd, &info) != 0 || std::cmp_not_equal(info.st_size, size)) {
        return std::nullopt;
    }
    const int seals = ::fcntl(fd, F_GET_SEALS);
    if (seals < 0 || (seals & F_SEAL_WRITE) == 0 || (seals & F_SEAL_SHRINK) == 0) {
        return std::nullopt;
    }
    std::size_t got = 0;
    while (got < size) {
        const ssize_t read = ::pread(fd, bytes.data() + got, size - got, static_cast<off_t>(got));
        if (read < 0 && errno == EINTR) {
            continue;
        }
        if (read <= 0) {
            return std::nullopt;
        }
        got += static_cast<std::size_t>(read);
    }
    return bytes;
}

}  // namespace

namespace {

// Closes a helper's socket (it leaves at its next read), makes sure it is gone, and waits for it:
// nothing left behind, not even a zombie.
// Returns its wait status.
int end(pid_t pid, int socket) {
    ::close(socket);
    // A helper that has already left keeps its exit status: killing a zombie changes nothing.
    ::kill(pid, SIGKILL);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return status;
}

}  // namespace

Helpers::Helpers(HelperOptions options)
    : options_{std::move(options)}, reaper_{[this] { close_idle(); }} {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    // Sanitizer builds (tests, snapshots) decode several times slower.
    options_.timeout *= 5;
#endif
}

Helpers::~Helpers() {
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
    }
    idle_changed_.notify_all();
    reaper_.join();
    const std::scoped_lock lock{mutex_};
    for (const Helper& helper : idle_) {
        end(helper.pid, helper.socket);
    }
}

void Helpers::close_idle() {
    std::unique_lock lock{mutex_};
    while (!stopping_) {
        if (idle_.empty()) {
            idle_changed_.wait(lock);
            continue;
        }
        const auto oldest = std::ranges::min_element(idle_, {}, &Helper::idle_since);
        const auto deadline = oldest->idle_since + options_.idle;
        if (std::chrono::steady_clock::now() < deadline) {
            idle_changed_.wait_until(lock, deadline);
            continue;
        }
        const Helper expired = *oldest;
        idle_.erase(oldest);
        --count_;
        lock.unlock();
        end(expired.pid, expired.socket);
        freed_.notify_one();
        lock.lock();
    }
}

std::size_t Helpers::running() const {
    const std::scoped_lock lock{mutex_};
    return count_;
}

std::optional<Helpers::Helper> Helpers::spawn() const {
    if (options_.program.empty()) {
        errno = ENOENT;
        return std::nullopt;
    }
    std::array<int, 2> ends{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends.data()) != 0) {
        return std::nullopt;
    }
    posix_spawn_file_actions_t actions{};
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, ends[1], protocol::kSocketFd);
    posix_spawn_file_actions_addclosefrom_np(&actions, protocol::kSocketFd + 1);
    posix_spawnattr_t attr{};
    posix_spawnattr_init(&attr);
    sigset_t none{};
    sigemptyset(&none);
    sigset_t all{};
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    // rummage closes idle helpers itself; a helper's own idle time, longer, is only a fallback.
    std::vector<std::string> args{options_.program.string(), "--idle-ms",
                                  std::to_string((options_.idle.count() * 2) + 1000)};
    args.insert(args.end(), options_.args.begin(), options_.args.end());
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int spawned =
        ::posix_spawn(&pid, args.front().c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    ::close(ends[1]);
    if (spawned != 0) {
        ::close(ends[0]);
        errno = spawned;
        return std::nullopt;
    }
    return Helper{.pid = pid, .socket = ends[0], .idle_since = {}};
}

std::optional<Helpers::Helper> Helpers::take(const core::CancelToken* cancel) {
    std::unique_lock lock{mutex_};
    while (true) {
        if (unavailable_ || (cancel != nullptr && cancel->cancelled())) {
            return std::nullopt;
        }
        // Idle ones that left meanwhile (their idle time) are let go of here.
        std::erase_if(idle_, [this](const Helper& helper) {
            int status = 0;
            if (::waitpid(helper.pid, &status, WNOHANG) != helper.pid) {
                return false;
            }
            ::close(helper.socket);
            --count_;
            return true;
        });
        if (!idle_.empty()) {
            const Helper helper = idle_.back();
            idle_.pop_back();
            return helper;
        }
        if (count_ < options_.max_helpers) {
            ++count_;
            lock.unlock();
            std::optional<Helper> started = spawn();
            if (!started) {
                const int error = errno;
                lock.lock();
                --count_;
                unavailable_ =
                    error == ENOENT || error == ENOTDIR || error == EACCES || error == ENOEXEC;
                freed_.notify_all();
            }
            return started;
        }
        freed_.wait_for(lock, std::chrono::milliseconds{50});
    }
}

void Helpers::give_back(Helper helper) {
    helper.idle_since = std::chrono::steady_clock::now();
    {
        const std::scoped_lock lock{mutex_};
        idle_.push_back(helper);
    }
    freed_.notify_one();
    idle_changed_.notify_one();
}

int Helpers::discard(Helper helper) {
    const int status = end(helper.pid, helper.socket);
    {
        const std::scoped_lock lock{mutex_};
        --count_;
        if (WIFEXITED(status) &&
            (WEXITSTATUS(status) == 127 || WEXITSTATUS(status) == protocol::kSandboxFailedExit)) {
            unavailable_ = true;
        }
    }
    freed_.notify_all();
    return status;
}

Helpers::Outcome Helpers::exchange(Helper helper, std::span<const std::byte> request,
                                   std::span<const int> fds, protocol::Packet& reply,
                                   std::string& why, const core::CancelToken* cancel) const {
    if (!protocol::send_packet(helper.socket, request, fds)) {
        return Outcome::again;  // it had already left
    }
    pollfd watched{.fd = helper.socket, .events = POLLIN, .revents = 0};
    const auto deadline = std::chrono::steady_clock::now() + options_.timeout;
    int ready = 0;
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (cancel != nullptr && cancel->cancelled()) {
            why = "preview cancelled";
            return Outcome::failed;
        }
        ready =
            ::poll(&watched, 1, static_cast<int>(std::clamp<std::int64_t>(left.count(), 0, 50)));
        if (ready == 0 && std::chrono::steady_clock::now() < deadline) {
            continue;
        }
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (ready == 0) {
        why = "the preview helper took longer than " + std::to_string(options_.timeout.count()) +
              " ms";
        return Outcome::failed;
    }
    std::optional<protocol::Packet> packet = protocol::receive_packet(helper.socket);
    if (!packet) {
        why = "the preview helper stopped";
        return Outcome::failed;
    }
    std::uint32_t magic = 0;
    if (packet->length >= sizeof(magic)) {
        std::memcpy(&magic, packet->bytes.data(), sizeof(magic));
    }
    if (magic == protocol::kGoodbyeMagic) {
        packet->close_fds();
        return Outcome::again;  // it was leaving when the request came
    }
    reply = *packet;
    return Outcome::done;
}

bool Helpers::run(std::span<const std::byte> request, std::span<const int> fds,
                  const std::function<bool(protocol::Packet&, std::string&)>& accept,
                  std::string& why, const core::CancelToken* cancel) {
    why = "the preview helper could not start";
    // Twice at most: a helper that had just left gets the request again in a new one.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::optional<Helper> helper = take(cancel);
        if (!helper) {
            why = options_.program.empty()
                      ? "rummage-preview not found"
                      : "the preview helper could not start: " + options_.program.string();
            return false;
        }
        protocol::Packet reply;
        Outcome outcome = exchange(*helper, request, fds, reply, why, cancel);
        if (outcome == Outcome::done) {
            const bool accepted = accept(reply, why);
            reply.close_fds();
            if (accepted) {
                give_back(*helper);
                return true;
            }
            outcome = Outcome::failed;  // it said something else: not to be trusted again
        }
        const int status = discard(*helper);
        if (WIFEXITED(status) && WEXITSTATUS(status) == protocol::kSandboxFailedExit) {
            why = "the preview helper could not set up its sandbox";
            return false;
        }
        if (outcome == Outcome::failed) {
            return false;
        }
    }
    return false;
}

picture::Loaded Helpers::load(const picture::LoadRequest& request) {
    const int file = open_regular(request.path);
    if (file < 0) {
        return failed(picture::Failure::unreadable, errno_text());
    }
    const protocol::Request message{.magic = protocol::kRequestMagic,
                                    .kind = protocol::Kind::picture,
                                    .max_width = request.max_width,
                                    .max_height = request.max_height};
    picture::Loaded result;
    const auto accept = [&](protocol::Packet& reply, std::string& why) {
        protocol::Response response;
        std::uint32_t magic = 0;
        std::memcpy(&magic, reply.bytes.data(), sizeof(magic));
        if (magic != protocol::kResponseMagic || reply.length < sizeof(response)) {
            why = "the preview helper said something else";
            return false;
        }
        std::memcpy(&response, reply.bytes.data(), sizeof(response));
        if (reply.length != sizeof(response) + response.detail_length ||
            response.failure > static_cast<std::uint8_t>(picture::Failure::locked)) {
            why = "the preview helper said something else";
            return false;
        }
        result.failure = static_cast<picture::Failure>(response.failure);
        result.from_preview = response.from_preview != 0;
        result.full_width = response.full_width;
        result.full_height = response.full_height;
        result.detail.assign(reinterpret_cast<const char*>(  // NOLINT: bytes to chars
                                 reply.bytes.data() + sizeof(response)),
                             response.detail_length);
        if (result.failure == picture::Failure::none) {
            std::optional<picture::Image> pixels =
                reply.fds[0] >= 0 ? read_pixels(reply.fds[0], response.width, response.height)
                                  : std::nullopt;
            if (!pixels) {
                why = "the preview helper's pixels are wrong";
                return false;
            }
            result.image = std::move(*pixels);
        }
        return true;
    };
    std::string why;
    const std::array<int, 1> fds{file};
    const bool done = run(std::as_bytes(std::span{&message, 1}), fds, accept, why, request.cancel);
    ::close(file);
    return done ? result : failed(picture::Failure::unavailable, std::move(why));
}

picture::PhotoInfo Helpers::photo_info(const std::filesystem::path& path) {
    const int file = open_regular(path);
    if (file < 0) {
        return {};
    }
    const protocol::Request message{.magic = protocol::kRequestMagic,
                                    .kind = protocol::Kind::photo_info,
                                    .max_width = 0,
                                    .max_height = 0};
    picture::PhotoInfo info;
    const auto accept = [&](protocol::Packet& reply, std::string& why) {
        std::optional<picture::PhotoInfo> decoded =
            protocol::decode_info(std::span{reply.bytes}.first(reply.length));
        if (!decoded) {
            why = "the preview helper said something else";
            return false;
        }
        info = std::move(*decoded);
        return true;
    };
    std::string why;
    const std::array<int, 1> fds{file};
    const bool done = run(std::as_bytes(std::span{&message, 1}), fds, accept, why);
    ::close(file);
    return done ? info : picture::PhotoInfo{};
}

text::Loaded Helpers::load_text(const text::LoadRequest& request) {
    const int file = open_regular(request.path);
    if (file < 0) {
        return {.failure = text::Failure::unreadable, .detail = errno_text(), .text = {}};
    }
    const std::vector<std::byte> message = protocol::encode_text_request(
        request.limits, request.path.filename().string(), request.mime);
    text::Loaded result;
    const auto accept = [&](protocol::Packet& reply, std::string& why) {
        protocol::TextResponse response;
        if (reply.length < sizeof(response)) {
            why = "the preview helper said something else";
            return false;
        }
        std::memcpy(&response, reply.bytes.data(), sizeof(response));
        if (response.magic != protocol::kTextMagic ||
            reply.length != sizeof(response) + response.detail_length + response.language_length ||
            response.failure > static_cast<std::uint8_t>(text::Failure::unreadable) ||
            response.encoding > static_cast<std::uint8_t>(text::Encoding::cp1252) ||
            response.truncated > 1 || response.crlf > 1 || response.tail > 1 ||
            (response.lines > 0 && response.failure != 0)) {
            why = "the preview helper said something else";
            return false;
        }
        const auto* chars =
            reinterpret_cast<const char*>(reply.bytes.data() + sizeof(response));  // NOLINT
        result.failure = static_cast<text::Failure>(response.failure);
        result.detail.assign(chars, response.detail_length);
        result.text.language.assign(chars + response.detail_length, response.language_length);
        result.text.encoding = static_cast<text::Encoding>(response.encoding);
        result.text.truncated = response.truncated != 0;
        result.text.crlf = response.crlf != 0;
        result.text.tail = response.tail != 0;
        if (result.failure == text::Failure::none) {
            // A line is at least its '\n' in the file.
            const std::uint64_t most_lines = std::min<std::uint64_t>(
                request.limits.max_lines == 0 ? request.limits.max_bytes : request.limits.max_lines,
                request.limits.max_bytes);
            const std::optional<std::vector<std::byte>> payload =
                response.lines > most_lines
                    ? std::nullopt
                    : read_sealed(reply.fds[0],
                                  protocol::text_payload_size(response.bytes, response.spans));
            if (!payload || !protocol::decode_text_payload(
                                *payload, response.lines, response.bytes, response.spans,
                                !result.text.language.empty(), request.limits, result.text)) {
                why = "the preview helper's text is wrong";
                return false;
            }
        }
        return true;
    };
    std::string why;
    const std::array<int, 1> fds{file};
    const bool done = run(message, fds, accept, why);
    ::close(file);
    if (!done) {
        return {.failure = text::Failure::unavailable, .detail = std::move(why), .text = {}};
    }
    return result;  // moved: a conditional expression here would copy every line
}

void use_engines(const engines::Registry& registry, HelperOptions options) {
    // One set of helpers per engine, shared by what it makes.
    std::vector<std::pair<const engines::Engine*, std::shared_ptr<Helpers>>> started;
    const auto helpers_of = [&](const engines::Engine& engine) {
        for (const auto& [which, helpers] : started) {
            if (which == &engine) {
                return helpers;
            }
        }
        HelperOptions own = options;
        own.program = engine.program;
        if (engine.manifest.name == "media" || engine.manifest.name == "pdf") {
            own.timeout = std::chrono::seconds{8};
            own.max_helpers = std::min<std::size_t>(own.max_helpers, 2);
        }
        started.emplace_back(&engine, std::make_shared<Helpers>(std::move(own)));
        return started.back().second;
    };
    const auto first_showing = [&](engines::Shows shows) -> const engines::Engine* {
        for (const engines::Engine& engine : registry.engines()) {
            if (std::ranges::any_of(engine.manifest.families, [&](const engines::Family& family) {
                    return family.shows == shows;
                })) {
                return &engine;
            }
        }
        return nullptr;
    };

    // Route by the caller's MIME type; empty requests retain the base image loader.
    auto routes = std::make_shared<const engines::Registry>(registry);
    const auto mime = fs::MimeDatabase::load();
    std::vector<std::shared_ptr<Helpers>> pools;
    for (const auto& engine : registry.engines()) {
        pools.push_back(helpers_of(engine));
    }
    picture::set_loader([routes, mime, pools](const picture::LoadRequest& request) {
        const engines::Engine* selected = nullptr;
        if (!request.mime.empty()) {
            if (const auto* family = routes->find(*mime, mime->find(request.mime));
                family != nullptr && family->shows == engines::Shows::picture) {
                selected = routes->engine_of(*family);
            }
        } else {
            for (const auto& engine : routes->engines()) {
                if (std::ranges::any_of(engine.manifest.families, [](const auto& family) {
                        return family.shows == engines::Shows::picture;
                    })) {
                    selected = &engine;
                    break;
                }
            }
        }
        if (selected != nullptr) {
            const auto index = static_cast<std::size_t>(selected - routes->engines().data());
            return pools[index]->load(request);
        }
        return failed(picture::Failure::unavailable, "no installed engine for this media type");
    });
    if (const engines::Engine* engine = first_showing(engines::Shows::picture)) {
        if (std::ranges::any_of(engine->manifest.families, &engines::Family::info)) {
            auto helpers = helpers_of(*engine);
            picture::set_info_loader(
                [helpers](const auto& path) { return helpers->photo_info(path); });
        }
    }
    if (const engines::Engine* engine = first_showing(engines::Shows::text)) {
        std::shared_ptr<Helpers> helpers = helpers_of(*engine);
        text::set_loader(
            [helpers](const text::LoadRequest& request) { return helpers->load_text(request); });
    } else {
        log::warn("no engine reads text: no previews of text");
    }
}

}  // namespace rummage::isolate
