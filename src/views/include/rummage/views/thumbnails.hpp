#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "rummage/core/worker_pool.hpp"
#include "rummage/picture/image.hpp"
#include "rummage/picture/load.hpp"
#include "rummage/thumbs/cache.hpp"
#include "rummage/ui/texture.hpp"

namespace rummage::views {

// Thumbnails for the views, as GPU textures. A view asks for the files it draws, every frame;
// the ones not ready are made on workers through the freedesktop cache (visible priority), the
// ones no longer asked for are cancelled, and the textures beyond a memory budget are dropped,
// least recently drawn first.
//
// Loop thread only. Textures are created inside get(), so call it while drawing (GL current).
class Thumbnails {
public:
    static constexpr std::size_t kBudgetBytes = std::size_t{256} << 20;

    Thumbnails(core::WorkerPool& pool, std::shared_ptr<const thumbs::Cache> cache,
               std::function<void()> redraw);
    ~Thumbnails();

    Thumbnails(const Thumbnails&) = delete;
    Thumbnails& operator=(const Thumbnails&) = delete;
    Thumbnails(Thumbnails&&) = delete;
    Thumbnails& operator=(Thumbnails&&) = delete;

    // The thumbnail of file (absolute) as it was at modified_ns, at size; nullptr while it is
    // being made (now asked for) or when it cannot be made. When it cannot, failure (if given)
    // says why, when the file says so (damaged, password protected); else none.
    [[nodiscard]] const ui::Texture* get(const std::filesystem::path& file,
                                         std::int64_t modified_ns, thumbs::Size size,
                                         std::string_view mime = {},
                                         picture::Failure* failure = nullptr);

    // After each frame: cancels what this frame did not ask for, keeps to the budget.
    void end_frame();

    // Thumbnails are being made (snapshots, tests).
    [[nodiscard]] bool loading() const noexcept { return in_flight_ > 0; }

    [[nodiscard]] std::size_t texture_bytes() const noexcept { return bytes_; }

    // What it holds (the memory report).
    struct Usage {
        std::size_t slots{0};
        std::size_t textures{0};
        std::size_t texture_bytes{0};
        std::size_t pixel_bytes{0};  // made, not uploaded yet
    };

    [[nodiscard]] Usage usage() const;

private:
    struct Slot {
        std::optional<core::CancelToken> job;    // being made
        std::shared_ptr<picture::Image> pixels;  // made, not uploaded yet
        std::unique_ptr<ui::Texture> texture;
        bool failed{false};
        picture::Failure failure{picture::Failure::none};  // why, when failed
        std::uint64_t used{0};  // the frame that last asked for it
    };

    core::WorkerPool& pool_;
    std::shared_ptr<const thumbs::Cache> cache_;
    std::function<void()> redraw_;
    std::unordered_map<std::string, Slot> slots_;  // by file, size and mtime
    std::uint64_t frame_{1};
    std::size_t bytes_{0};
    std::size_t in_flight_{0};
    // False once destroyed: results still on their way find out here.
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
};

}  // namespace rummage::views
