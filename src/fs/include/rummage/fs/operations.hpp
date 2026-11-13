#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Changes to the file system the user asks for: new folders and renames. Blocking: run them on a
// worker. They never replace an existing entry.
namespace rummage::fs {

// Why name cannot name an entry ("A name cannot contain /"); nullopt when it can.
[[nodiscard]] std::optional<std::string> name_problem(std::string_view name);

struct MadeFolder {
    std::string name;  // empty on error
    std::error_code error;
};

// Makes a folder in directory called base, else "base 2", "base 3"...: the first free name,
// taken atomically (mkdir fails on an existing one, and the next is tried).
[[nodiscard]] MadeFolder make_folder(const std::filesystem::path& directory, std::string_view base);

struct MadeFile {
    std::string name;  // empty on error
    std::error_code error;
};

// Makes a file in directory called name, else "name 2" ("Letter 2.odt": the number goes before
// the extension), "name 3"...: the first free name, taken atomically (O_EXCL). Empty, or with
// the contents and permissions of from (a template); a copy that fails leaves nothing.
[[nodiscard]] MadeFile make_file(const std::filesystem::path& directory, std::string_view name,
                                 const std::filesystem::path& from = {});

// The templates "New File" offers: the regular files in directory (links to them too), not
// hidden nor backups ("~"), sorted by name; empty when there is no such folder.
[[nodiscard]] std::vector<std::string> templates_in(const std::filesystem::path& directory);

// The darktable sidecars of a file among names (the directory's entries), sorted: "X.ORF.xmp" and
// the duplicates "X_01.ORF.xmp" (an underscore and two or more digits before the file's
// extension), unless a file of the directory has the duplicate's name ("X_1234.ORF": the sidecar
// is its own).
[[nodiscard]] std::vector<std::string> sidecars_of(std::span<const std::string> names,
                                                   std::string_view name);

// The name a sidecar of from (one sidecars_of gave) takes when from becomes to.
[[nodiscard]] std::string sidecar_name(std::string_view sidecar, std::string_view from,
                                       std::string_view to);

// The darktable sidecars that go with a file and the names they take when it is renamed to to:
// "X.ORF.xmp" becomes "Y.ORF.xmp", and duplicates "X_01.ORF.xmp" (an underscore and two or more
// digits before the file's extension) become "Y_01.ORF.xmp", unless a file of the directory has
// the duplicate's name ("X_1234.ORF": the sidecar is its own). names: the directory's entries.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> sidecar_renames(
    std::span<const std::string> names, std::string_view from, std::string_view to);

struct Renamed {
    std::error_code error;  // the entry itself could not be renamed: nothing changed
    // Ready to show when error is set or a sidecar could not follow: "X.ORF.xmp already exists".
    std::string problem;
    std::size_t sidecars{0};  // sidecars renamed with it
};

// Renames directory/from to directory/to without replacing anything (renameat2 with
// RENAME_NOREPLACE; on file systems without it, a check first). A regular file takes its darktable
// sidecars along; if one of their new names is taken, nothing is renamed.
[[nodiscard]] Renamed rename_entry(const std::filesystem::path& directory, const std::string& from,
                                   const std::string& to);

}  // namespace rummage::fs
