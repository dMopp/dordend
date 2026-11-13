#include "sandbox.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <string_view>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <system_error>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

namespace rummage::isolate {
namespace {

#if defined(__x86_64__)
constexpr std::uint32_t kArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t kArch = AUDIT_ARCH_AARCH64;
#else
#  error "the preview helper's seccomp filter knows x86_64 and aarch64 only"
#endif

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

}  // namespace

#if defined(__SANITIZE_ADDRESS__)
// LeakSanitizer checks at exit with a tracer (clone and ptrace): the cage forbids both, so the
// processes that live in it (the helper, the test probe) leave the leak check to rummage's own.
extern "C" int __lsan_is_turned_off() { return 1; }  // NOLINT(readability-identifier-naming)
#endif

namespace {

// 4 GiB of address space: a 200 MP picture (the decoders' limit) is 800 MB of pixels, plus the
// file and the decoders' buffers.
constexpr rlim_t kAddressSpace = rlim_t{4} << 30;
constexpr rlim_t kFiles = 32;

// The system calls a picture needs (measured with strace on JPEG, PNG, WebP, JPEG XL, ORF and
// DNG), and the helper's own: its socket, its memfd, malloc_trim.
constexpr std::array kAllowed{
    // reading the file rummage sent, and talking to rummage
    __NR_read,
    __NR_pread64,
    __NR_write,
    __NR_recvmsg,
    __NR_sendmsg,
    __NR_ppoll,
#ifdef __NR_poll
    __NR_poll,
#endif
    __NR_lseek,
    __NR_close,
    __NR_fcntl,
    __NR_fstat,
    // memory and the pixels' memfd
    __NR_mmap,
    __NR_munmap,
    __NR_mremap,
    __NR_mprotect,
    __NR_brk,
    __NR_madvise,
    __NR_memfd_create,
    __NR_ftruncate,
    // what the C and C++ libraries use underneath
    __NR_futex,
    __NR_getrandom,
    __NR_clock_gettime,
    __NR_rt_sigreturn,
    __NR_rt_sigprocmask,
    __NR_exit,
    __NR_exit_group,
};

// Sanitizer builds (tests only): their runtimes need a few more (ASan checks whether memory is
// readable through a pipe: pipe2 and ioctl, seen in a snapshot with RUMMAGE_SANDBOX_REPORT=1).
constexpr std::array kAllowedSanitized{
    __NR_sigaltstack,       __NR_gettid,    __NR_getpid,
    __NR_sched_yield,       __NR_prctl,     __NR_rt_sigaction,
    __NR_sched_getaffinity, __NR_tgkill,    __NR_nanosleep,
    __NR_clock_nanosleep,   __NR_getrlimit, __NR_prlimit64,
    __NR_readlinkat,        __NR_pipe2,     __NR_ioctl,
#ifdef __NR_readlink
    __NR_readlink,
#endif
};

// Opening a path fails with EACCES: a library that tries gets an error, not a dead helper.
constexpr std::array kRefused{
#ifdef __NR_open
    __NR_open,
#endif
    __NR_openat,
    __NR_openat2,
};

using Program = std::vector<sock_filter>;

void add(Program& program, std::uint16_t code, std::uint32_t k, std::uint8_t jt = 0,
         std::uint8_t jf = 0) {
    program.push_back(sock_filter{.code = code, .jt = jt, .jf = jf, .k = k});
}

// Loads 32 bits of seccomp_data at offset.
void load(Program& program, std::size_t offset) {
    add(program, BPF_LD | BPF_W | BPF_ABS, static_cast<std::uint32_t>(offset));
}

void ret(Program& program, std::uint32_t action) { add(program, BPF_RET | BPF_K, action); }

Program filter(Unexpected unexpected) {
    const std::uint32_t otherwise =
        unexpected == Unexpected::kill ? SECCOMP_RET_KILL_PROCESS : SECCOMP_RET_TRAP;
    const std::uint32_t refuse = SECCOMP_RET_ERRNO | (EACCES & SECCOMP_RET_DATA);
    Program program;
    // Another architecture's numbers (32-bit or x32 calls): never.
    load(program, offsetof(seccomp_data, arch));
    add(program, BPF_JMP | BPF_JEQ | BPF_K, kArch, 1, 0);
    ret(program, SECCOMP_RET_KILL_PROCESS);
    load(program, offsetof(seccomp_data, nr));
#if defined(__x86_64__)
    add(program, BPF_JMP | BPF_JGE | BPF_K, 0x40000000U /* __X32_SYSCALL_BIT */, 0, 1);
    ret(program, SECCOMP_RET_KILL_PROCESS);
#endif
    const auto allow_each = [&](const auto& numbers) {
        for (const auto number : numbers) {
            add(program, BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(number), 0, 1);
            ret(program, SECCOMP_RET_ALLOW);
        }
    };
    allow_each(kAllowed);
    if constexpr (kSanitized) {
        allow_each(kAllowedSanitized);
    }
    for (const auto number : kRefused) {
        add(program, BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(number), 0, 1);
        ret(program, refuse);
    }
    // newfstatat only on a descriptor (AT_EMPTY_PATH): glibc's fstat, never a path. The flags
    // are the fourth argument; their low 32 bits are enough.
    add(program, BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(__NR_newfstatat), 0, 4);
    load(program, offsetof(seccomp_data, args) + (3 * sizeof(std::uint64_t)));
    add(program, BPF_JMP | BPF_JSET | BPF_K, AT_EMPTY_PATH, 0, 1);
    ret(program, SECCOMP_RET_ALLOW);
    ret(program, refuse);
    ret(program, otherwise);
    return program;
}

// RUMMAGE_SANDBOX_REPORT: a call the filter does not know is written to stderr and fails with
// ENOSYS, so that a library's new needs show up instead of a dead helper.
void report(int /*signal*/, siginfo_t* info, void* context) {
    constexpr std::string_view prefix = "rummage-preview: system call not allowed: ";
    std::array<char, 80> line{};
    std::ranges::copy(prefix, line.begin());
    char* const digits = line.data() + prefix.size();
    char* end = std::to_chars(digits, line.data() + line.size() - 1, info->si_syscall).ptr;
    *end = '\n';
    static_cast<void>(
        ::write(STDERR_FILENO, line.data(), static_cast<std::size_t>(end - line.data() + 1)));
    auto* frame = static_cast<ucontext_t*>(context);
#if defined(__x86_64__)
    frame->uc_mcontext.gregs[REG_RAX] = -ENOSYS;
#elif defined(__aarch64__)
    frame->uc_mcontext.regs[0] = static_cast<std::uint64_t>(-ENOSYS);
#endif
}

}  // namespace

int restrict_file_system_and_network() {
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return 0;
    }
    const long abi =
        ::syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) {
        return 0;
    }
    landlock_ruleset_attr attr{};
    attr.handled_access_fs =
        LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;
    if (abi >= 2) {
        attr.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
    }
    if (abi >= 3) {
        attr.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
    }
    if (abi >= 4) {
        attr.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP | LANDLOCK_ACCESS_NET_CONNECT_TCP;
    }
    if (abi >= 5) {
        attr.handled_access_fs |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    }
    if (abi >= 6) {
        attr.scoped = LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET | LANDLOCK_SCOPE_SIGNAL;
    }
    // The size the running kernel knows for its ABI: older kernels refuse fields they lack.
    std::size_t size = sizeof(attr);
    if (abi < 4) {
        size = offsetof(landlock_ruleset_attr, handled_access_net);
    } else if (abi < 6) {
        size = offsetof(landlock_ruleset_attr, scoped);
    }
    const long ruleset = ::syscall(__NR_landlock_create_ruleset, &attr, size, 0);
    if (ruleset < 0) {
        return 0;
    }
    // No rule added: nothing in the file system is reachable.
    const long restricted = ::syscall(__NR_landlock_restrict_self, static_cast<int>(ruleset), 0);
    ::close(static_cast<int>(ruleset));
    return restricted == 0 ? static_cast<int>(abi) : 0;
}

SandboxStatus enter_sandbox(Unexpected unexpected) {
    SandboxStatus status;
    const rlimit no_core{.rlim_cur = 0, .rlim_max = 0};
    const rlimit files{.rlim_cur = kFiles, .rlim_max = kFiles};
    const rlimit space{.rlim_cur = kAddressSpace, .rlim_max = kAddressSpace};
    // The sanitizers reserve terabytes of shadow address space: no limit on it there.
    status.limited = ::setrlimit(RLIMIT_CORE, &no_core) == 0 &&
                     ::setrlimit(RLIMIT_NOFILE, &files) == 0 &&
                     (kSanitized || ::setrlimit(RLIMIT_AS, &space) == 0);
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        status.error = "no_new_privs: " + std::error_code{errno, std::generic_category()}.message();
        return status;
    }
    status.landlock_abi = restrict_file_system_and_network();
    if (unexpected == Unexpected::report) {
        struct sigaction action{};
        action.sa_sigaction = report;
        action.sa_flags = SA_SIGINFO;
        ::sigaction(SIGSYS, &action, nullptr);
    }
    Program program = filter(unexpected);
    const sock_fprog fprog{.len = static_cast<unsigned short>(program.size()),
                           .filter = program.data()};
    if (::syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) != 0) {
        status.error = "seccomp: " + std::error_code{errno, std::generic_category()}.message();
        return status;
    }
    status.seccomp = true;
    return status;
}

}  // namespace rummage::isolate
