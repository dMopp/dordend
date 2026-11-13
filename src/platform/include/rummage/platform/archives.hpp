#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/core/actions.hpp"
#include "rummage/platform/desktop_entry.hpp"

namespace rummage::platform {

struct ArchiveBackend {
    enum class Kind { file_roller, seven_zip, unrar, unzip, gzip, tar };
    Kind kind{};
    std::filesystem::path program;
    bool tar_layer{false};  // 7z needs a separate pass for compressed tar
    bool single_file{false};
};

// Prefer unrar for RAR, then 7zz/7z for ZIP/RAR/7z; otherwise File Roller,
// format-specific CLI, then 7zz/7z.
// All programs are optional.
[[nodiscard]] std::optional<ArchiveBackend> archive_backend(const core::ActionTarget& target,
                                                            const Environment& environment);
[[nodiscard]] std::string archive_output_name(const std::filesystem::path& archive);
// CLI commands operate in a fresh directory. stdout is redirected for gzip / first tar pass.
[[nodiscard]] std::vector<std::string> archive_command(const ArchiveBackend& backend,
                                                       const std::filesystem::path& archive,
                                                       bool stream = false,
                                                       bool password_stdin = false);

// Human-readable guidance shared by missing-program and runtime failures.
[[nodiscard]] std::string archive_install_hint(const core::ActionTarget& target);
[[nodiscard]] bool archive_password_error(const ArchiveBackend& backend, int status,
                                          std::string_view diagnostic);
[[nodiscard]] std::string archive_failure(const core::ActionTarget& target,
                                          const ArchiveBackend& backend, std::string_view operation,
                                          int status, int signal, std::string_view diagnostic,
                                          const std::filesystem::path& destination = {},
                                          const std::filesystem::path& staging = {},
                                          std::string_view password = {});

}  // namespace rummage::platform
