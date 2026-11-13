#include <unordered_set>

#include "rummage/views/context_menu.hpp"

namespace rummage::views {

std::vector<MenuItem> arrange_menu(const std::vector<MenuItem>& available,
                                   const std::vector<core::MenuNode>& layout) {
    std::unordered_set<std::string> placed;
    const auto collect = [&](const auto& self, const std::vector<core::MenuNode>& nodes) -> void {
        for (const auto& node : nodes) {
            if (!node.id.empty()) {
                placed.insert(node.id);
            }
            self(self, node.items);
        }
    };
    collect(collect, layout);
    const auto find = [&](const auto& self, const std::vector<MenuItem>& items,
                          const std::string& id) -> const MenuItem* {
        for (const auto& item : items) {
            if (item.id == id) {
                return &item;
            }
            if (const auto* found = self(self, item.children, id)) {
                return found;
            }
        }
        return nullptr;
    };
    const auto remainder = [&](const auto& self,
                               const std::vector<MenuItem>& items) -> std::vector<MenuItem> {
        std::vector<MenuItem> result;
        for (auto item : items) {
            if (placed.contains(item.id)) {
                continue;
            }
            if (item.submenu) {
                item.children = self(self, item.children);
                if (item.children.empty() && item.empty_text.empty() && item.id != "new_file") {
                    continue;
                }
            }
            item.separator = !result.empty() && item.separator;
            result.push_back(std::move(item));
        }
        return result;
    };
    const auto build = [&](const auto& self,
                           const std::vector<core::MenuNode>& nodes) -> std::vector<MenuItem> {
        std::vector<MenuItem> result;
        bool separator = false;
        const auto append = [&](MenuItem item) {
            item.separator = !result.empty() && (separator || item.separator);
            separator = false;
            result.push_back(std::move(item));
        };
        for (const auto& node : nodes) {
            if (node.remaining) {
                separator = separator || node.separator;
                for (auto item : remainder(remainder, available)) {
                    append(std::move(item));
                }
            } else if (!node.id.empty()) {
                if (const auto* source = find(find, available, node.id)) {
                    auto item = *source;
                    if (item.submenu) {
                        item.children = remainder(remainder, item.children);
                        if (item.children.empty() && item.empty_text.empty() &&
                            item.id != "new_file") {
                            continue;
                        }
                    }
                    if (!node.label.empty()) {
                        item.label = node.label;
                    }
                    item.separator = node.separator;
                    append(std::move(item));
                }
            } else if (!node.label.empty()) {
                MenuItem group;
                group.label = node.label;
                group.submenu = true;
                group.separator = node.separator;
                group.children = self(self, node.items);
                if (!group.children.empty()) {
                    append(std::move(group));
                }
            } else {
                separator = separator || node.separator;
            }
        }
        return result;
    };
    return build(build, layout);
}

}  // namespace rummage::views
