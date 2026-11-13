// rummage-preview: rummage's preview helper (ROADMAP 2b). Started by rummage with a socket on
// descriptor 3, never by hand: the base engine (data/engines/base.toml). It reads files only
// through the descriptors rummage sends, turns them into pictures (the image family) or texts
// (the text family), and answers with the pixels or the lines in a sealed memfd. It leaves after
// --idle-ms without work (a goodbye first), or when rummage closes the socket.

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <malloc.h>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include "backend.hpp"
#include "rummage/picture/load.hpp"
#include "protocol.hpp"
#include "sandbox.hpp"

namespace isolate = rummage::isolate;
namespace protocol = rummage::isolate::protocol;
namespace picture = rummage::picture;

namespace {

constexpr int kTrimAfterMs = 1000;

// A memfd holding bytes, sealed so that neither side can change its size; -1 on an error.
int sealed_memfd(const char* name, std::span<const std::byte> bytes) {
    const int fd = ::memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        return -1;
    }
    const std::byte* from = bytes.data();
    std::size_t left = bytes.size();
    while (left > 0) {
        const ssize_t wrote = ::write(fd, from, left);
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            ::close(fd);
            return -1;
        }
        from += wrote;
        left -= static_cast<std::size_t>(wrote);
    }
    if (::fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void answer_picture(int socket, int file, const protocol::Request& request) {
    const picture::Loaded loaded =
        isolate::backend::make_picture(file, request.max_width, request.max_height);
    protocol::Response response;
    response.failure = static_cast<std::uint8_t>(loaded.failure);
    response.from_preview = loaded.from_preview ? 1 : 0;
    response.full_width = loaded.full_width;
    response.full_height = loaded.full_height;
    int pixels = -1;
    if (loaded.failure == picture::Failure::none) {
        pixels = sealed_memfd("rummage-picture", std::as_bytes(std::span{loaded.image.rgba}));
        if (pixels < 0) {
            response.failure = static_cast<std::uint8_t>(picture::Failure::unavailable);
        } else {
            response.width = loaded.image.width;
            response.height = loaded.image.height;
        }
    }
    const std::string_view detail = std::string_view{loaded.detail}.substr(0, protocol::kMaxDetail);
    response.detail_length = static_cast<std::uint16_t>(detail.size());
    std::array<std::byte, sizeof(protocol::Response) + protocol::kMaxDetail> bytes{};
    std::memcpy(bytes.data(), &response, sizeof(response));
    std::memcpy(bytes.data() + sizeof(response), detail.data(), detail.size());
    const std::array<int, 1> fds{pixels};
    static_cast<void>(
        protocol::send_packet(socket, std::span{bytes}.first(sizeof(response) + detail.size()),
                              pixels >= 0 ? std::span<const int>{fds} : std::span<const int>{}));
    if (pixels >= 0) {
        ::close(pixels);
    }
}

void answer_text(int socket, int file, const protocol::TextAsk& ask) {
    rummage::text::Loaded loaded = isolate::backend::read_text(file, ask.limits);
    if (loaded.failure == rummage::text::Failure::none) {
        isolate::backend::colour(loaded.text, ask.name, ask.mime);
    }
    protocol::TextResponse response;
    response.failure = static_cast<std::uint8_t>(loaded.failure);
    response.encoding = static_cast<std::uint8_t>(loaded.text.encoding);
    response.truncated = loaded.text.truncated ? 1 : 0;
    response.crlf = loaded.text.crlf ? 1 : 0;
    response.tail = loaded.text.tail ? 1 : 0;
    std::string_view language;
    int payload = -1;
    if (loaded.failure == rummage::text::Failure::none) {
        language = std::string_view{loaded.text.language}.substr(0, protocol::kMaxField);
        std::uint32_t text_bytes = 0;
        std::uint32_t spans = 0;
        const std::vector<std::byte> all =
            protocol::encode_text_payload(loaded.text, text_bytes, spans);
        payload = all.empty() ? -1 : sealed_memfd("rummage-text", all);
        if (!all.empty() && payload < 0) {
            response.failure = static_cast<std::uint8_t>(rummage::text::Failure::unreadable);
            language = {};
        } else {
            response.lines = static_cast<std::uint32_t>(loaded.text.lines.size());
            response.bytes = text_bytes;
            response.spans = spans;
        }
    }
    const std::string_view detail = std::string_view{loaded.detail}.substr(0, protocol::kMaxDetail);
    response.detail_length = static_cast<std::uint16_t>(detail.size());
    response.language_length = static_cast<std::uint8_t>(language.size());
    std::array<std::byte,
               sizeof(protocol::TextResponse) + protocol::kMaxDetail + protocol::kMaxField>
        bytes{};
    std::memcpy(bytes.data(), &response, sizeof(response));
    std::memcpy(bytes.data() + sizeof(response), detail.data(), detail.size());
    std::memcpy(bytes.data() + sizeof(response) + detail.size(), language.data(), language.size());
    const std::array<int, 1> fds{payload};
    static_cast<void>(protocol::send_packet(
        socket, std::span{bytes}.first(sizeof(response) + detail.size() + language.size()),
        payload >= 0 ? std::span<const int>{fds} : std::span<const int>{}));
    if (payload >= 0) {
        ::close(payload);
    }
}

void answer_photo_info(int socket, int file) {
    const std::vector<std::byte> bytes = protocol::encode_info(isolate::backend::photo_info(file));
    static_cast<void>(protocol::send_packet(socket, bytes));
}

bool debug_log() {
    const char* level = std::getenv("RUMMAGE_LOG");  // NOLINT(concurrency-mt-unsafe): one thread
    return level != nullptr &&
           (std::string_view{level} == "debug" || std::string_view{level} == "trace");
}

}  // namespace

int main(int argc, char** argv) {
    int idle_ms = 30'000;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string_view arg{
            argv[i]};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if (arg == "--idle-ms") {
            const std::string_view value{
                argv[i + 1]};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            std::from_chars(value.data(), value.data() + value.size(), idle_ms);
        }
    }
    // When rummage goes away its end of the socket closes, and the helper leaves at its next
    // read. (Not PR_SET_PDEATHSIG: that follows the thread that started the helper, and
    // rummage's threads come and go.)
    std::signal(SIGPIPE, SIG_IGN);  // NOLINT(cert-err33-c): nothing to check

    const pid_t pid = ::getpid();  // asked now: getpid is not allowed in the cage
    isolate::backend::warm_up();
    const char* report = std::getenv("RUMMAGE_SANDBOX_REPORT");  // NOLINT(concurrency-mt-unsafe)
    const bool reporting = report != nullptr && std::string_view{report} == "1";
    const isolate::SandboxStatus sandbox =
        isolate::enter_sandbox(reporting ? isolate::Unexpected::report : isolate::Unexpected::kill);
    if (!sandbox.seccomp) {
        std::fprintf(stderr, "rummage-preview: sandbox could not be set up: %s\n",
                     sandbox.error.c_str());
        return protocol::kSandboxFailedExit;
    }
    if (debug_log()) {
        std::fprintf(stderr, "rummage-preview %d: seccomp on, landlock %s, limits %s\n",
                     static_cast<int>(pid),
                     sandbox.landlock_abi > 0
                         ? ("ABI " + std::to_string(sandbox.landlock_abi)).c_str()
                         : "not available",
                     sandbox.limited ? "set" : "not set");
    }
    const int socket = protocol::kSocketFd;
    // After a picture, memory is given back once a second passes without another (a burst of
    // thumbnails keeps its buffers: trimming after each one cost a 1920x1440 ORF 64 -> 85 ms);
    // then the helper waits out the rest of its idle time and leaves.
    bool holding = false;
    int idle_left = idle_ms;  // until it leaves, counted from the last picture
    while (true) {
        const int wait_ms = holding ? std::min(kTrimAfterMs, idle_left) : idle_left;
        pollfd watched{.fd = socket, .events = POLLIN, .revents = 0};
        const int ready = ::poll(&watched, 1, wait_ms);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0 && holding) {
            ::malloc_trim(0);
            holding = false;
            idle_left = std::max(idle_left - wait_ms, 0);
            continue;
        }
        if (ready == 0) {
            const protocol::Request goodbye{.magic = protocol::kGoodbyeMagic};
            static_cast<void>(protocol::send_packet(socket, std::as_bytes(std::span{&goodbye, 1})));
            return 0;
        }
        std::optional<protocol::Packet> packet = protocol::receive_packet(socket);
        if (!packet) {
            return 0;  // rummage closed the socket
        }
        // Its magic and kind first, then the whole request for that kind.
        std::uint32_t magic = 0;
        protocol::Kind kind{};
        const int file = packet->fds[0];
        if (packet->length < sizeof(magic) + sizeof(kind) || file < 0) {
            packet->close_fds();
            return 1;  // not rummage talking
        }
        std::memcpy(&magic, packet->bytes.data(), sizeof(magic));
        std::memcpy(&kind, packet->bytes.data() + sizeof(magic), sizeof(kind));
        const std::span<const std::byte> bytes = std::span{packet->bytes}.first(packet->length);
        protocol::Request request;
        bool understood = magic == protocol::kRequestMagic;
        if (understood && kind == protocol::Kind::text) {
            const std::optional<protocol::TextAsk> ask = protocol::decode_text_request(bytes);
            understood = ask.has_value();
            if (ask) {
                answer_text(socket, file, *ask);
            }
        } else if (understood && packet->length == sizeof(request)) {
            std::memcpy(&request, packet->bytes.data(), sizeof(request));
            if (request.kind == protocol::Kind::photo_info) {
                answer_photo_info(socket, file);
            } else if (request.kind == protocol::Kind::picture) {
                answer_picture(socket, file, request);
            } else {
                understood = false;
            }
        } else {
            understood = false;
        }
        if (!understood) {
            packet->close_fds();
            return 1;
        }
        packet->close_fds();
        holding = true;  // 54 MB kept after 16 ORF pictures, until trimmed
        idle_left = idle_ms;
    }
}
