#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rummage::core {

struct Action {
    enum class Kind { external, copy_location, extract_here, extract_to };
    enum class Selection { one, many, background, any };
    enum class Placement { actions, menu };
    enum class Destination { none, file, directory };
    std::string id;
    std::string label;
    std::vector<std::string> argv;
    std::vector<std::string> mime;
    Selection selection{Selection::one};
    Kind kind{Kind::external};
    bool enabled{true};
    Placement placement{Placement::actions};
    Destination destination{Destination::none};
    std::string destination_name{};  // suggested filename for the save picker
    bool notify_on_completion{false};
};

struct ActionTarget {
    std::filesystem::path path;
    std::string mime;
};

// Captured when a menu/palette is built, or a shortcut is pressed. No live selection pointers.
struct ActionContext {
    std::filesystem::path directory;
    std::vector<ActionTarget> targets;
    bool trash{false};
};

// User definitions replace bundled external actions with the same ID.
[[nodiscard]] std::vector<Action> action_registry(std::span<const Action> custom = {});
[[nodiscard]] std::optional<std::string> action_problem(const Action& action);
[[nodiscard]] bool action_matches(const Action& action, const ActionContext& context);
// Only whole arguments are expanded. {files} becomes several arguments, never shell text.
[[nodiscard]] std::vector<std::string> action_arguments(
    const Action& action, const ActionContext& context,
    const std::filesystem::path& destination = {});

}  // namespace rummage::core
