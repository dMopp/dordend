#include "rummage/picture/load.hpp"

#include <mutex>
#include <utility>

namespace rummage::picture {

std::string_view failure_message(Failure failure) noexcept {
    switch (failure) {
        case Failure::none:
            return {};
        case Failure::unknown_format:
            return "rummage can't show this kind of file";
        case Failure::no_readable_preview:
            return "No preview rummage can show in this file";
        case Failure::too_large:
            return "This image is too large to show";
        case Failure::damaged:
            return "This image looks damaged";
        case Failure::unreadable:
            return "This file can't be read";
        case Failure::unavailable:
            return "Preview unavailable";
    }
    return {};
}

namespace {
std::mutex loader_mutex;
Loader current_loader;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): set once
}  // namespace

void set_loader(Loader loader) {
    const std::scoped_lock lock{loader_mutex};
    current_loader = std::move(loader);
}

Loaded load_picture(const LoadRequest& request) {
    Loader loader;
    {
        const std::scoped_lock lock{loader_mutex};
        loader = current_loader;
    }
    if (!loader) {
        Loaded none;
        none.failure = Failure::unavailable;
        none.detail = "no picture loader";
        return none;
    }
    return loader(request);
}

namespace {
std::mutex info_loader_mutex;
InfoLoader current_info_loader;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace

void set_info_loader(InfoLoader loader) {
    const std::scoped_lock lock{info_loader_mutex};
    current_info_loader = std::move(loader);
}

PhotoInfo load_photo_info(const std::filesystem::path& path) {
    InfoLoader loader;
    {
        const std::scoped_lock lock{info_loader_mutex};
        loader = current_info_loader;
    }
    return loader ? loader(path) : PhotoInfo{};
}

}  // namespace rummage::picture
