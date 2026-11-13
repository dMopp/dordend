#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/core/worker_pool.hpp"
#include "rummage/fs/entry.hpp"
#include "rummage/fs/mime.hpp"
#include "rummage/platform/applications.hpp"
#include "rummage/platform/desktop_entry.hpp"
#include "rummage/platform/launcher.hpp"

namespace rummage::platform {

// A file to open, by absolute path, and its type.
struct OpenFile {
    std::filesystem::path path;
    std::string mime;
};

// What opening some files does: the processes to start, and what cannot be opened.
struct OpenPlan {
    struct Launch {
        std::string app_id;
        std::string app_name;  // the localized Name, else the ID
        std::vector<std::string> argv;
        std::filesystem::path directory;  // the entry's Path, else the first file's folder
        std::size_t files{0};             // how many of the files it opens
    };

    std::vector<Launch> launches;
    // Ready to show: "No application opens image/x-foo", "No terminal for Helix".
    std::vector<std::string> problems;
    std::size_t files{0};    // the files with an application
    std::string first_name;  // the first of them, by name
};

// Groups files by their default application (in the order they come) and expands each group's
// Exec line: files for one application go to one process when it takes a list. Terminal=true
// applications run inside in_terminal(). With app_id, every file goes to that application
// ("Open With").
[[nodiscard]] OpenPlan plan_open(std::span<const OpenFile> files, const Applications& apps,
                                 const fs::MimeDatabase& database, const Environment& environment,
                                 std::string_view app_id = {});

// An application to offer for a file ("Open With").
struct AppChoice {
    std::string id;
    std::string name;  // the localized Name, else the ID
    bool is_default{false};
};

// The applications for a type: the default first, then Applications::all_for's.
[[nodiscard]] std::vector<AppChoice> choices_for(std::string_view mime, const Applications& apps,
                                                 const fs::MimeDatabase& database);

// Opens files with their default applications (DESIGN, "Apertura dei file"): types and
// applications are worked out on a worker, the processes start on the loop thread, each with an
// activation token when one can be had, and what happened is reported in one line.
//
// Loop thread only. Must not outlive the loop or the pool.
class Opener {
public:
    // A fresh xdg-activation token for the next process, handed to done (at once with "" when
    // there is none to be had).
    using TokenRequest = std::function<void(std::function<void(std::string token)> done)>;
    // A line for the user: "Opening photo.orf with darktable…", or an error.
    using Report = std::function<void(std::string message, bool error)>;

    Opener(core::EventLoop& loop, core::WorkerPool& pool,
           std::shared_ptr<const fs::MimeDatabase> database, Environment environment, Report report,
           TokenRequest token = nullptr);
    ~Opener();

    Opener(const Opener&) = delete;
    Opener& operator=(const Opener&) = delete;
    Opener(Opener&&) = delete;
    Opener& operator=(Opener&&) = delete;

    // Opens directory's entries (not folders: callers go into those). Entries with a known type
    // keep it; the others are detected as the views would.
    // With app_id, all of them with that application, folders too ("Open With" on a folder).
    void open(const std::filesystem::path& directory, std::vector<fs::Entry> entries,
              std::string app_id = {});

    // Opens a terminal (terminal_alone()) with its shell in directory: "Opening Documents with
    // foot…", or "No terminal found: set $TERMINAL".
    void open_terminal(const std::filesystem::path& directory);

    // The applications for entry (its type detected if unknown), worked out on a worker and
    // handed to done on the loop thread.
    void choices(const std::filesystem::path& directory, fs::Entry entry,
                 std::function<void(std::vector<AppChoice>)> done);

    // A process that ends with an error sooner than this after it started is reported: the
    // application could not open the file. Later exits are its own business.
    static constexpr std::chrono::seconds kQuickExit{2};

    [[nodiscard]] Launcher& launcher() noexcept { return launcher_; }

    // A plan is being worked out (tests).
    [[nodiscard]] bool planning() const noexcept { return planning_ > 0; }

private:
    void start(OpenPlan plan);
    void start_one(const OpenPlan::Launch& launch, const std::string& token);

    core::WorkerPool& pool_;
    std::shared_ptr<const fs::MimeDatabase> database_;
    std::shared_ptr<const Environment> environment_;
    Report report_;
    TokenRequest token_;
    Launcher launcher_;
    std::size_t planning_{0};
    // False once destroyed: plans and tokens still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::platform
