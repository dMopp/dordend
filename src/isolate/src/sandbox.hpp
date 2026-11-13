#pragma once

// The preview helper's cage (ROADMAP 2b-2), our own code, no library: resource limits, landlock
// (no file system, no network, no signals or abstract sockets towards other processes), and a
// seccomp filter that lets through only what decoding a picture and talking to rummage need.
// Opening a file fails with EACCES; any other system call kills the process.

#include <cstdint>
#include <string>

namespace rummage::isolate {

struct SandboxStatus {
    bool limited{false};  // resource limits set
    int landlock_abi{0};  // the landlock version enforced; 0: not available
    bool seccomp{false};  // the filter is in force
    std::string error;    // why the cage could not be built (seccomp is required)
};

// What the filter does with a system call it does not allow.
enum class Unexpected : std::uint8_t {
    kill,    // the process dies (SIGSYS): the normal case
    report,  // RUMMAGE_SANDBOX_REPORT=1: its number goes to stderr and it fails with ENOSYS
};

// Landlock alone (tests): no file system, no TCP, no abstract sockets or signals outward. The
// ABI enforced, 0 when not available.
[[nodiscard]] int restrict_file_system_and_network();

// Builds the cage around this process, for good. Single-threaded processes only. On failure the
// caller must not go on (the helper exits).
[[nodiscard]] SandboxStatus enter_sandbox(Unexpected unexpected);

}  // namespace rummage::isolate
