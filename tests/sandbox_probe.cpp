// Tries one thing inside the preview helper's cage and says what happened, for
// tests/sandbox_test.cpp. Its argument says what: allowed (what a picture needs), open, stat,
// socket, exec, fork, memory (5 GiB), or landlock (landlock alone: a file and a TCP connection).
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <netinet/in.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "sandbox.hpp"

namespace isolate = rummage::isolate;

namespace {

// Printed with write: stdio's buffers would need nothing more, but stay simple.
void say(std::string_view text) {
    static_cast<void>(::write(STDOUT_FILENO, text.data(), text.size()));
    static_cast<void>(::write(STDOUT_FILENO, "\n", 1));
}

void say_errno(int result) {
    if (result >= 0) {
        say("ok");
    } else if (errno == EACCES) {
        say("EACCES");
    } else {
        say("other error");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    const std::string_view what{
        argv[1]};  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (what == "landlock") {
        // A file sure to exist on any system (no /etc/hostname in Nix's build sandbox): the
        // probe itself, so landlock refuses it rather than the lookup failing first.
        std::error_code error;
        const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", error);
        if (error) {
            say("no /proc/self/exe");
            return 1;
        }
        if (isolate::restrict_file_system_and_network() == 0) {
            say("no landlock");
            return 0;
        }
        say_errno(::open(self.c_str(), O_RDONLY | O_CLOEXEC));
        const int tcp = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(9);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        say_errno(::connect(tcp, reinterpret_cast<sockaddr*>(&address),  // NOLINT: sockets
                            sizeof(address)));
        return 0;
    }
    const isolate::SandboxStatus status = isolate::enter_sandbox(isolate::Unexpected::kill);
    if (!status.seccomp) {
        say("no sandbox");
        return 1;
    }
    if (what == "allowed") {
        const int fd = ::memfd_create("probe", MFD_CLOEXEC);
        void* memory = std::malloc(std::size_t{64} << 20);  // NOLINT: the allocator's calls
        std::memset(memory, 1, std::size_t{64} << 20);
        std::free(memory);  // NOLINT
        struct stat info{};
        const bool fine = fd >= 0 && ::ftruncate(fd, 4096) == 0 && ::fstat(fd, &info) == 0 &&
                          info.st_size == 4096;
        say(fine ? "ok" : "failed");
    } else if (what == "open") {
        say_errno(::open("/etc/hostname", O_RDONLY | O_CLOEXEC));
    } else if (what == "stat") {
        struct stat info{};
        say_errno(::stat("/etc", &info));
    } else if (what == "socket") {
        say_errno(::socket(AF_INET, SOCK_STREAM, 0));  // killed before it returns
    } else if (what == "exec") {
        ::execl("/bin/true", "true", nullptr);  // killed
        say("returned");
    } else if (what == "fork") {
        say_errno(::fork());  // killed
    } else if (what == "memory") {
        void* huge = ::mmap(nullptr, std::size_t{5} << 30, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        say(huge == MAP_FAILED ? "refused" : "granted");
    }
    return 0;
}
