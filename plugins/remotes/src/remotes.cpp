// The "remotes" plugin: remote bookmarks (smb://, sftp://) mounted through gvfs on the session
// bus, answered on the Browser's password and question cards.

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "nordend/plugin/plugin.hpp"
#include "nordend/plugins/remotes/gvfs.hpp"

namespace nordend::plugins::remotes {
namespace {

// What gvfs asks before a mount, answered on the Browser's cards.
Gvfs::Prompts gvfs_prompts(views::Remotes::Prompts prompts) {
    return {
        .password =
            [login = std::move(prompts.login)](const PasswordRequest& request,
                                               std::function<void(const PasswordAnswer&)> answer) {
                if (!login) {
                    answer({});
                    return;
                }
                const auto asks = [&request](std::uint32_t flag) {
                    return (request.flags & flag) != 0;
                };
                login({.message = request.message,
                       .user = request.user,
                       .domain = request.domain,
                       .ask_user = asks(PasswordRequest::kNeedUsername),
                       .ask_domain = asks(PasswordRequest::kNeedDomain),
                       .ask_password = asks(PasswordRequest::kNeedPassword),
                       .anonymous = asks(PasswordRequest::kAnonymousSupported),
                       .saving = asks(PasswordRequest::kSavingSupported)},
                      [answer = std::move(answer)](const std::optional<views::Login>& given) {
                          PasswordAnswer out;
                          if (given) {
                              out = {.aborted = false,
                                     .anonymous = given->anonymous,
                                     .user = given->user,
                                     .domain = given->domain,
                                     .password = given->password,
                                     .remember = given->remember};
                          }
                          answer(out);
                      });
            },
        .question = std::move(prompts.question),
        .cancel = std::move(prompts.cancel),
    };
}

class RemotesPlugin final : public plugin::Plugin {
public:
    explicit RemotesPlugin(plugin::Host& host) : gvfs_{host.loop, host.places_changed} {}

    [[nodiscard]] std::vector<std::string> schemes() const override { return {"smb", "sftp"}; }

    [[nodiscard]] bool can_mount() const override { return gvfs_.available(); }

    void mount(const std::string& uri, views::Remotes::Prompts prompts,
               views::Remotes::Mounted done) override {
        gvfs_.mount(uri, gvfs_prompts(std::move(prompts)), std::move(done));
    }

    void unmount(const std::string& uri, views::Remotes::Done done) override {
        gvfs_.unmount(uri, std::move(done));
    }

    [[nodiscard]] std::vector<fs::Place> places(std::vector<fs::Place> places) const override {
        return with_remote_mounts(std::move(places), gvfs_.mounts());
    }

private:
    Gvfs gvfs_;
};

}  // namespace
}  // namespace nordend::plugins::remotes

// Found by the generated plugin list (src/plugin/builtin.cpp.in).
std::unique_ptr<nordend::plugin::Plugin> nordend_plugin_make_remotes(nordend::plugin::Host& host) {
    return std::make_unique<nordend::plugins::remotes::RemotesPlugin>(host);
}
