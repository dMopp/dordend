#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "rummage/core/actions.hpp"
#include "rummage/core/file_picker.hpp"
#include "rummage/platform/archives.hpp"
#include "rummage/platform/desktop_entry.hpp"
#include "rummage/platform/launcher.hpp"
#include "rummage/platform/notifications.hpp"

namespace rummage::platform {

// External applications own their work, with an optional rummage destination picker before
// handoff. Built-in CLI archive actions track extraction and publish completed output
// without replacing existing files.
class ActionHandoff {
public:
    using Report = std::function<void(std::string, bool)>;
    using Activate = std::function<void(std::function<void(std::string)>)>;
    using DirectoryChosen = std::function<void(std::optional<std::filesystem::path>)>;
    using ChooseDirectory = std::function<void(const std::filesystem::path&, DirectoryChosen)>;
    using ChooseDestination = std::function<void(const core::PickerRequest&, DirectoryChosen)>;
    using Notify = std::function<void(const std::string&, const std::string&, bool)>;
    using PasswordChosen = std::function<void(std::optional<std::string>)>;
    using AskPassword =
        std::function<void(const std::filesystem::path&, bool retry, PasswordChosen)>;
    ActionHandoff(core::EventLoop& loop, Environment environment, Report report,
                  Activate activate = {}, ChooseDirectory choose = {},
                  ChooseDestination destination_picker = {}, Notify notify = {});
    ~ActionHandoff();

    void set_password_prompt(AskPassword password) { password_ = std::move(password); }

    [[nodiscard]] bool available(const std::string& program) const;
    [[nodiscard]] bool available(const core::Action& action,
                                 const core::ActionContext& context) const;
    void run(const core::Action& action, const core::ActionContext& context);

private:
    struct Extraction;
    void launch(const core::Action& action, const core::ActionContext& context,
                const std::filesystem::path& destination = {}, bool archive = false);
    void run_archive(const core::Action& action, const core::ActionContext& context);
    void choose_destination(const std::filesystem::path& initial, DirectoryChosen chosen);
    void choose_path(const core::PickerRequest& request, DirectoryChosen chosen);
    void extract(const core::ActionTarget& target, const ArchiveBackend& backend,
                 const std::filesystem::path& destination);
    void extract_step(std::shared_ptr<Extraction> extraction, bool second_pass = false);
    void probe_archive(std::shared_ptr<Extraction> extraction, bool verify = false);
    void ask_password(std::shared_ptr<Extraction> extraction);
    void next_password();
    void fail_archive(const std::shared_ptr<Extraction>& job, std::string_view operation,
                      const Launcher::Exit& result, int output_fd = -1);
    void archive_problem(const std::shared_ptr<Extraction>& job, std::string reason,
                         std::string guidance);
    Launcher launcher_;
    Environment environment_;
    Report report_;
    Activate activate_;
    ChooseDirectory choose_;
    ChooseDestination destination_picker_;
    Notifications notifications_;
    Notify notify_;
    AskPassword password_;
    std::deque<std::shared_ptr<Extraction>> passwords_;
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::platform
