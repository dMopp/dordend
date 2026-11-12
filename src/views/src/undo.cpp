#include "nordend/views/undo.hpp"
#include "nordend/core/i18n.hpp"

#include <format>

namespace nordend::views {

std::string describe(const FileOperation& operation) {
    const std::size_t count = operation.items.size();
    std::string what = count == 1
                           ? std::format("“{}”", operation.items.front().to.filename().string())
                           : i18n::trf("{} items", count);
    switch (operation.kind) {
        case FileOperation::Kind::rename:
            if (count == 1) {
                return i18n::trf("“{}” is “{}” again",
                                   operation.items.front().from.filename().string(),
                                   operation.items.front().to.filename().string());
            }
            break;
        case FileOperation::Kind::trash:
            if (count == 1) {
                return i18n::trf("“{}” moved to the trash",
                                   operation.items.front().from.filename().string());
            }
            return i18n::trf("{} moved to the trash", what);
        case FileOperation::Kind::restore:
            return i18n::trf("{} restored", what);
        case FileOperation::Kind::copy:
        case FileOperation::Kind::move:
            if (count > 0) {
                return i18n::trf("{} back in {}", what,
                                   operation.items.front().to.parent_path().filename().string());
            }
            break;
        case FileOperation::Kind::create:
            return i18n::trf("{} made", what);
        case FileOperation::Kind::erase:
            break;
    }
    return what;
}

}  // namespace nordend::views
