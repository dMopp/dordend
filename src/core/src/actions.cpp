#include "rummage/core/actions.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace rummage::core {

std::vector<Action> action_registry(std::span<const Action> custom) {
    const std::vector<std::string> archives{"application/zip",
                                            "application/x-7z-compressed",
                                            "application/vnd.rar",
                                            "application/x-rar",
                                            "application/x-rar-compressed",
                                            "application/x-tar",
                                            "application/x-compressed-tar",
                                            "application/x-bzip-compressed-tar",
                                            "application/x-bzip2-compressed-tar",
                                            "application/x-xz-compressed-tar",
                                            "application/x-zstd-compressed-tar",
                                            "application/gzip",
                                            "application/x-gzip",
                                            "application/x-bzip2",
                                            "application/x-xz",
                                            "application/zstd"};
    std::vector<Action> actions{{.id = "copy_location",
                                 .label = "Copy Location",
                                 .argv = {},
                                 .mime = {},
                                 .selection = Action::Selection::any,
                                 .kind = Action::Kind::copy_location,
                                 .placement = Action::Placement::menu},
                                {.id = "extract_here",
                                 .label = "Extract Here",
                                 .argv = {},
                                 .mime = archives,
                                 .selection = Action::Selection::many,
                                 .kind = Action::Kind::extract_here,
                                 .placement = Action::Placement::menu},
                                {.id = "extract_to",
                                 .label = "Extract To…",
                                 .argv = {},
                                 .mime = archives,
                                 .selection = Action::Selection::many,
                                 .kind = Action::Kind::extract_to,
                                 .placement = Action::Placement::menu}};
    for (const auto& action : custom) {
        const auto found = std::ranges::find(actions, action.id, &Action::id);
        if (found == actions.end()) {
            actions.push_back(action);
        } else if (found->kind != Action::Kind::copy_location) {
            *found = action;
        }
    }
    return actions;
}

std::optional<std::string> action_problem(const Action& action) {
    if (action.id.empty() ||
        action.id.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos) {
        return "id must contain only letters, digits, '_' or '-'";
    }
    if (action.id == "copy_location") {
        return "copy_location is a built-in action and cannot be replaced";
    }
    if (action.label.empty() || action.label.find('\0') != std::string::npos) {
        return "label must be nonempty and contain no NUL";
    }
    if (action.kind == Action::Kind::external &&
        (action.argv.empty() || action.argv.front().empty())) {
        return "argv must start with a program";
    }
    if (action.destination != Action::Destination::none && action.kind != Action::Kind::external) {
        return "destination is only supported for external actions";
    }
    if (action.notify_on_completion && action.kind != Action::Kind::external) {
        return "notify_on_completion is only supported for external actions";
    }
    if (!action.destination_name.empty() && action.destination != Action::Destination::file) {
        return "destination_name requires destination = 'file'";
    }
    if (action.destination_name.find('/') != std::string::npos ||
        action.destination_name.find('\0') != std::string::npos || action.destination_name == "." ||
        action.destination_name == "..") {
        return "destination_name must be a filename, not a path";
    }
    const bool has_destination =
        std::ranges::find(action.argv, "{destination}") != action.argv.end();
    if (has_destination != (action.destination != Action::Destination::none)) {
        return "{destination} and a destination picker must be configured together";
    }
    constexpr std::array<std::string_view, 4> fields{"{file}", "{files}", "{directory}",
                                                     "{destination}"};
    for (std::size_t i = 0; i < action.argv.size(); ++i) {
        const auto& arg = action.argv[i];
        if (arg.find('\0') != std::string::npos) {
            return "argv must contain no NUL";
        }
        for (const auto field : fields) {
            if (arg.find(field) != std::string::npos && (arg != field || i == 0)) {
                return "placeholders must be whole arguments after the program";
            }
        }
        if (arg.starts_with('{') && arg.ends_with('}') &&
            std::ranges::find(fields, arg) == fields.end()) {
            return "unknown argument placeholder";
        }
        if (arg == "{file}" && action.selection != Action::Selection::one) {
            return "{file} requires selection = 'one'";
        }
        if (arg == "{files}" && (action.selection == Action::Selection::background ||
                                 action.selection == Action::Selection::any)) {
            return "{files} requires selection = 'one' or 'many'";
        }
    }
    for (const auto& type : action.mime) {
        const auto slash = type.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 == type.size() ||
            type.find('\0') != std::string::npos ||
            (type.find('*') != std::string::npos &&
             (type.substr(slash + 1) != "*" || type.find('*') != slash + 1))) {
            return "mime must contain MIME types or type/* patterns";
        }
    }
    return std::nullopt;
}

bool action_matches(const Action& action, const ActionContext& context) {
    if (!action.enabled || context.trash || context.directory.empty()) {
        return false;
    }
    const auto count = context.targets.size();
    if ((action.selection == Action::Selection::one && count != 1) ||
        (action.selection == Action::Selection::many && count == 0) ||
        (action.selection == Action::Selection::background && count != 0)) {
        return false;
    }
    const auto matches_type = [&](std::string_view type) {
        return action.mime.empty() || std::ranges::any_of(action.mime, [&](const auto& pattern) {
                   return pattern == type || (pattern.ends_with("/*") &&
                                              type.starts_with(std::string_view{pattern}.substr(
                                                  0, pattern.size() - 1)));
               });
    };
    if (count == 0) {
        return matches_type("inode/directory");
    }
    return std::ranges::all_of(context.targets,
                               [&](const auto& target) { return matches_type(target.mime); });
}

std::vector<std::string> action_arguments(const Action& action, const ActionContext& context,
                                          const std::filesystem::path& destination) {
    if (!action_matches(action, context) ||
        (action.destination != Action::Destination::none && destination.empty())) {
        return {};
    }
    std::vector<std::string> args;
    for (const auto& arg : action.argv) {
        if (arg == "{files}") {
            for (const auto& target : context.targets) {
                args.push_back(target.path.string());
            }
        } else if (arg == "{file}") {
            if (context.targets.size() != 1) {
                return {};
            }
            args.push_back(context.targets.front().path.string());
        } else if (arg == "{directory}") {
            args.push_back(context.directory.string());
        } else if (arg == "{destination}") {
            args.push_back(destination.string());
        } else {
            args.push_back(arg);
        }
    }
    return args;
}

}  // namespace rummage::core
