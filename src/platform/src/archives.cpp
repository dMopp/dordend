#include "rummage/platform/archives.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace rummage::platform {
namespace {

std::string lower_name(const std::filesystem::path& path) {
    auto name = path.filename().string();
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return name;
}

}  // namespace

std::optional<ArchiveBackend> archive_backend(const core::ActionTarget& target,
                                              const Environment& environment) {
    using Kind = ArchiveBackend::Kind;
    const auto find = [&](std::string_view program) {
        return find_program(program, environment.path);
    };
    const auto& mime = target.mime;
    const bool zip = mime == "application/zip";
    const bool rar = mime == "application/vnd.rar" || mime == "application/x-rar" ||
                     mime == "application/x-rar-compressed";
    // Some distributions build 7-Zip without the RAR decoder. Prefer unrar when available.
    if (rar) {
        if (const auto program = find("unrar"))
            return ArchiveBackend{.kind = Kind::unrar, .program = *program};
    }
    // Keep encrypted ZIP, RAR and 7z extraction in rummage's password flow.
    if (zip || rar || mime == "application/x-7z-compressed") {
        for (const auto* candidate : {"7zz", "7z"}) {
            if (const auto program = find(candidate)) {
                return ArchiveBackend{.kind = Kind::seven_zip, .program = *program};
            }
        }
    }
    if (const auto program = find("file-roller")) {
        return ArchiveBackend{.kind = Kind::file_roller, .program = *program};
    }
    const auto name = lower_name(target.path);
    const bool tgz = mime == "application/x-compressed-tar" || name.ends_with(".tar.gz") ||
                     name.ends_with(".tgz");
    const bool tbz = mime == "application/x-bzip2-compressed-tar" ||
                     mime == "application/x-bzip-compressed-tar" || name.ends_with(".tar.bz2") ||
                     name.ends_with(".tbz2") || name.ends_with(".tbz");
    const bool txz = mime == "application/x-xz-compressed-tar" || name.ends_with(".tar.xz") ||
                     name.ends_with(".txz");
    const bool tzst = mime == "application/x-zstd-compressed-tar" || name.ends_with(".tar.zst") ||
                      name.ends_with(".tzst");
    const bool tar = mime == "application/x-tar" || tgz || tbz || txz || tzst;
    const bool gzip = !tar && (mime == "application/gzip" || mime == "application/x-gzip");
    if (zip) {
        if (const auto program = find("unzip")) {
            return ArchiveBackend{.kind = Kind::unzip, .program = *program};
        }
    }
    if (tar && (!tgz || find("gzip")) && (!tbz || find("bzip2")) && (!txz || find("xz")) &&
        (!tzst || find("zstd"))) {
        if (const auto program = find("tar")) {
            return ArchiveBackend{.kind = Kind::tar, .program = *program};
        }
    }
    if (gzip) {
        if (const auto program = find("gzip")) {
            return ArchiveBackend{.kind = Kind::gzip, .program = *program, .single_file = true};
        }
    }
    const bool zstd = mime == "application/zstd" || tzst;
    if (zip || tar || gzip || mime == "application/x-7z-compressed" ||
        mime == "application/vnd.rar" || mime == "application/x-rar" ||
        mime == "application/x-rar-compressed" || mime == "application/x-bzip2" ||
        mime == "application/x-xz" || zstd) {
        for (const auto* candidate : {"7zz", "7z"}) {
            // Older p7zip's 7z does not support Zstandard.
            if (zstd && std::string_view{candidate} == "7z") {
                continue;
            }
            if (const auto program = find(candidate)) {
                return ArchiveBackend{
                    .kind = Kind::seven_zip,
                    .program = *program,
                    .tar_layer = tgz || tbz || txz || tzst,
                    .single_file = !tar && (gzip || mime == "application/x-bzip2" ||
                                            mime == "application/x-xz" || zstd)};
            }
        }
    }
    return std::nullopt;
}

std::string archive_output_name(const std::filesystem::path& archive) {
    const auto lower = lower_name(archive);
    auto name = archive.filename().string();
    for (const auto* suffix :
         {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tgz", ".tbz2", ".tbz", ".txz", ".tzst",
          ".zip", ".7z", ".rar", ".tar", ".gz", ".bz2", ".xz", ".zst"}) {
        if (lower.ends_with(suffix)) {
            name.resize(name.size() - std::string_view{suffix}.size());
            break;
        }
    }
    if (name.empty() || name == "." || name == "..") {
        name = "Extracted";
    }
    // Leave room for a collision suffix and the visible in-progress name.
    return name.substr(0, 180);
}

std::vector<std::string> archive_command(const ArchiveBackend& backend,
                                         const std::filesystem::path& archive, bool stream,
                                         bool password_stdin) {
    const auto program = backend.program.string();
    const auto path = archive.string();
    switch (backend.kind) {
        case ArchiveBackend::Kind::file_roller:
            return {program, "--extract-here", path};
        case ArchiveBackend::Kind::seven_zip: {
            // When needed, 7z reads the password from redirected stdin, never argv.
            std::vector<std::string> args{program, "x",     "-aos", "-y",       "-bd",
                                          "-bso0", "-bsp0", "-spd", "-sccUTF-8"};
            if (!password_stdin)
                args.push_back("-p-");
            if (stream || backend.single_file) {
                args.push_back("-so");
            }
            args.insert(args.end(), {"--", path});
            return args;
        }
        case ArchiveBackend::Kind::unrar:
            return {program, "x",   "-cfg-", "-c-",
                    "-idq",  "-o-", "-y",    password_stdin ? "-p" : "-p-",
                    "--",    path};
        case ArchiveBackend::Kind::unzip:
            // Info-ZIP also treats wildcards inside an argv as patterns. An inherited open
            // archive fd, supplied by the caller, gives it a literal /proc/self/fd path.
            return {program, "-n", "-qq", path};
        case ArchiveBackend::Kind::gzip:
            return {program, "-d", "-c", "--", path};
        case ArchiveBackend::Kind::tar:
            return {program,
                    "--extract",
                    "--keep-old-files",
                    "--no-same-owner",
                    "--no-same-permissions",
                    "--file",
                    path};
    }
    return {};
}

namespace {
std::string lower_text(std::string_view text) {
    std::string result{text};
    for (auto& c : result)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
    return result;
}

bool rar_target(const core::ActionTarget& target) {
    return target.mime == "application/vnd.rar" || target.mime == "application/x-rar" ||
           target.mime == "application/x-rar-compressed";
}
}  // namespace

std::string archive_install_hint(const core::ActionTarget& target) {
    if (rar_target(target))
        return "RAR extraction requires unrar or a RAR-capable 7-Zip build. Install unrar and try "
               "again.";
    if (target.mime == "application/zip" || target.mime == "application/x-7z-compressed")
        return "Install 7zz (7-Zip) or 7z and try again; these also support archive passwords.";
    return "Install 7zz (7-Zip), or tar with the matching gzip, bzip2, xz or zstd decompressor, "
           "and try again.";
}

bool archive_password_error(const ArchiveBackend& backend, int status,
                            std::string_view diagnostic) {
    const auto text = lower_text(diagnostic);
    // Unsupported encryption is not a wrong password; do not repeatedly ask for a secret.
    if (text.find("unsupported") != std::string::npos ||
        text.find("not supported") != std::string::npos)
        return false;
    return text.find("wrong password") != std::string::npos ||
           text.find("incorrect password") != std::string::npos ||
           (backend.kind == ArchiveBackend::Kind::unrar && status == 11) ||
           (backend.kind == ArchiveBackend::Kind::unzip && status == 82);
}

std::string archive_failure(const core::ActionTarget& target, const ArchiveBackend& backend,
                            std::string_view operation, int status, int signal,
                            std::string_view diagnostic, const std::filesystem::path& destination,
                            const std::filesystem::path& staging, std::string_view password) {
    const auto text = lower_text(diagnostic);
    const auto has = [&](std::string_view part) { return text.find(part) != std::string::npos; };
    const bool rar = rar_target(target);
    const bool unsupported = has("unsupported") || has("not supported") || has("unknown method");
    const bool password_error = archive_password_error(backend, status, diagnostic);
    std::string reason;
    if (signal != 0) {
        reason =
            "The extractor was stopped by a signal. Retry; if this repeats, check system resources "
            "or update the extractor.";
    } else if (unsupported) {
        reason =
            "The installed extractor does not support this archive's compression or encryption. " +
            archive_install_hint(target);
    } else if (has("unable to get password") || has("password required") ||
               (backend.kind == ArchiveBackend::Kind::unzip && password_error)) {
        reason =
            "This archive needs a password, but unzip cannot use rummage's password dialog. "
            "Install 7zz (7-Zip) and try again.";
    } else if (password_error) {
        reason =
            "The password was rejected, or encrypted data is damaged. Check the password and try "
            "again; if it still fails, obtain a fresh copy of the archive.";
    } else if (has("no space left") || has("disk full")) {
        reason =
            "There is not enough space to finish extraction. Free space on the destination drive "
            "or choose another destination.";
    } else if (has("permission denied") || has("access is denied") ||
               has("read-only file system")) {
        reason =
            "Access was denied. Check that the archive is readable and choose a writable "
            "destination.";
    } else if (backend.kind == ArchiveBackend::Kind::unrar && (status == 5 || status == 9)) {
        reason =
            "The extractor could not create or write output. Check destination permissions and "
            "free disk space.";
    } else if (has("missing volume") || has("next volume") || has("cannot find volume")) {
        reason =
            "An archive part is missing. Put all parts in the same folder and open the first part "
            "with an archive manager.";
    } else if (rar && backend.kind != ArchiveBackend::Kind::unrar &&
               (has("as archive") || has("as [rar") ||
                backend.kind == ArchiveBackend::Kind::file_roller)) {
        reason = "The archive may be damaged, or this extractor may lack RAR support. " +
                 archive_install_hint(target);
    } else if (has("unexpected end") || has("unexpected eof") || has("truncated") || has("crc") ||
               has("checksum") || has("data error") || has("headers error") ||
               has("not in gzip format") || has("as archive") || has("end-of-central-directory")) {
        reason =
            "The archive is damaged, incomplete, or not in the expected format. Obtain a fresh "
            "copy and make sure all archive parts are present.";
    } else if (status == 255 && (backend.kind == ArchiveBackend::Kind::seven_zip ||
                                 backend.kind == ArchiveBackend::Kind::unrar)) {
        reason =
            "The extractor was interrupted before completion. Retry if you did not intend to "
            "cancel extraction.";
    } else if (status == 8 && (backend.kind == ArchiveBackend::Kind::seven_zip ||
                               backend.kind == ArchiveBackend::Kind::unrar)) {
        reason =
            "The extractor ran out of memory. Close other applications or use a machine with more "
            "available memory.";
    } else {
        reason =
            "The extractor could not complete the operation. Check the details below, the "
            "archive's integrity, and destination permissions; then retry or use another "
            "extractor.";
        if (rar)
            reason += " " + archive_install_hint(target);
    }
    std::string details{diagnostic};
    if (!password.empty()) {
        std::size_t pos = 0;
        while ((pos = details.find(password, pos)) != std::string::npos) {
            details.replace(pos, password.size(), "[redacted]");
            pos += 10;
        }
    }
    // Keep line breaks for readable diagnostics; strip terminal controls and embedded NULs.
    for (auto& c : details)
        if ((static_cast<unsigned char>(c) < 32 && c != '\n' && c != '\t') || c == 127)
            c = ' ';
    std::string message = std::format(
        "Could not {} “{}”.\n{}\n\nArchive: {}\nExtractor: {}\nExit status: {}; signal: {}",
        operation, target.path.filename().string(), reason, target.path.string(),
        backend.program.string(), status, signal);
    if (!destination.empty())
        message += "\nDestination: " + destination.string();
    if (backend.kind == ArchiveBackend::Kind::file_roller)
        message += "\nCheck the destination chosen in File Roller for partial output.";
    else
        message += staging.empty() ? "\nNo extraction output was created."
                                   : "\nPartial output (not published): " + staging.string();
    message += "\n\nExtractor details:\n" +
               (details.empty() ? std::string{"The extractor did not provide diagnostic output."}
                                : details);
    return message;
}

}  // namespace rummage::platform
