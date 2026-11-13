#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace rummage::ui {

// An RGBA image on the GPU, with mipmaps so that it stays smooth when drawn smaller. Made from
// straight-alpha pixels (premultiplied on upload). Requires the GL context to be current for
// construction and destruction, like the Renderer: create textures while drawing.
class Texture {
public:
    // rgba: width * height * 4 bytes, rows top to bottom.
    Texture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&&) = delete;
    Texture& operator=(Texture&&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }

    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

    [[nodiscard]] unsigned id() const noexcept { return id_; }

    // GPU memory, mipmaps included (about 4/3 of the image).
    [[nodiscard]] std::size_t bytes() const noexcept;

private:
    unsigned id_{0};
    std::uint32_t width_;
    std::uint32_t height_;
};

}  // namespace rummage::ui
