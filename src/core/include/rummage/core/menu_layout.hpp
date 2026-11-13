#pragma once

#include <optional>
#include <string>
#include <vector>

namespace rummage::core {

// A reference to an available item, a named group, a separator, or the remaining items.
struct MenuNode {
    std::string id{};
    std::string label{};
    std::vector<MenuNode> items{};
    bool separator{false};
    bool remaining{false};
};

struct MenuLayout {
    // Missing layouts retain the default. An explicitly empty layout hides that menu.
    std::optional<std::vector<MenuNode>> items;
    std::optional<std::vector<MenuNode>> entry;
    std::optional<std::vector<MenuNode>> background;
    std::optional<std::vector<MenuNode>> trash;
    std::optional<std::vector<MenuNode>> sidebar;
};

}  // namespace rummage::core
