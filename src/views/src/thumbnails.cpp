#include "rummage/views/thumbnails.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace rummage::views {

Thumbnails::Thumbnails(core::WorkerPool& pool, std::shared_ptr<const thumbs::Cache> cache,
                       std::function<void()> redraw)
    : pool_{pool}, cache_{std::move(cache)}, redraw_{std::move(redraw)} {}

Thumbnails::~Thumbnails() {
    *alive_ = false;
    for (auto& [key, slot] : slots_) {
        if (slot.job) {
            slot.job->cancel();
        }
    }
}

const ui::Texture* Thumbnails::get(const std::filesystem::path& file, std::int64_t modified_ns,
                                   thumbs::Size size, std::string_view mime,
                                   picture::Failure* failure) {
    std::string key = file.string();
    key += '\n';
    key += thumbs::directory_name(size);
    key += '\n';
    key += std::to_string(modified_ns);
    key += mime;
    auto [it, added] = slots_.try_emplace(key);
    Slot& slot = it->second;
    slot.used = frame_;
    if (added) {
        ++in_flight_;
        slot.job = pool_.submit(
            core::Priority::visible,
            [cache = cache_, file, size, mime = std::string{mime}](const core::CancelToken& token) {
                thumbs::Thumbnail thumbnail = cache->get(file, size, mime, &token);
                return std::make_shared<thumbs::Thumbnail>(std::move(thumbnail));
            },
            [this, alive = alive_, key](std::shared_ptr<thumbs::Thumbnail> made) {
                if (!*alive) {
                    return;
                }
                const auto found = slots_.find(key);
                if (found == slots_.end()) {
                    return;  // dropped meanwhile
                }
                --in_flight_;
                found->second.job.reset();
                if (made->image.empty()) {
                    found->second.failed = true;
                    found->second.failure = made->failure;
                } else {
                    found->second.pixels =
                        std::make_shared<picture::Image>(std::move(made->image));
                }
                redraw_();
            });
        return nullptr;
    }
    if (failure != nullptr) {
        *failure = slot.failed ? slot.failure : picture::Failure::none;
    }
    if (slot.pixels) {
        slot.texture = std::make_unique<ui::Texture>(slot.pixels->width, slot.pixels->height,
                                                     slot.pixels->rgba);
        bytes_ += slot.texture->bytes();
        slot.pixels.reset();
    }
    return slot.texture.get();
}

void Thumbnails::end_frame() {
    // Not asked for this frame and still being made: no longer wanted.
    for (auto it = slots_.begin(); it != slots_.end();) {
        Slot& slot = it->second;
        if (slot.used != frame_ && (slot.job || slot.pixels)) {
            if (slot.job) {
                slot.job->cancel();
                --in_flight_;
            }
            it = slots_.erase(it);
        } else {
            ++it;
        }
    }
    // Over budget: drop the textures drawn longest ago (never this frame's).
    if (bytes_ > kBudgetBytes) {
        std::vector<std::pair<std::uint64_t, std::string>> by_age;
        for (const auto& [key, slot] : slots_) {
            if (slot.texture && slot.used != frame_) {
                by_age.emplace_back(slot.used, key);
            }
        }
        std::ranges::sort(by_age);
        for (const auto& [used, key] : by_age) {
            if (bytes_ <= kBudgetBytes) {
                break;
            }
            const auto it = slots_.find(key);
            if (it != slots_.end() && it->second.texture) {
                bytes_ -= it->second.texture->bytes();
                slots_.erase(it);
            }
        }
    }
    ++frame_;
}

Thumbnails::Usage Thumbnails::usage() const {
    Usage usage{.slots = slots_.size(), .textures = 0, .texture_bytes = bytes_, .pixel_bytes = 0};
    for (const auto& [key, slot] : slots_) {
        usage.textures += slot.texture ? 1U : 0U;
        usage.pixel_bytes += slot.pixels ? slot.pixels->rgba.size() : 0;
    }
    return usage;
}

}  // namespace rummage::views
