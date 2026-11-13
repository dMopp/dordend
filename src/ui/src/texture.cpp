#include "rummage/ui/texture.hpp"

#include <GLES3/gl3.h>
#include <vector>

namespace rummage::ui {

Texture::Texture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba)
    : width_{width}, height_{height} {
    // Premultiplied, as the renderer blends: filtering then never bleeds the color of
    // transparent pixels into the edges.
    std::vector<std::uint8_t> premultiplied(rgba.begin(), rgba.end());
    for (std::size_t i = 0; i + 3 < premultiplied.size(); i += 4) {
        const unsigned alpha = premultiplied[i + 3];
        if (alpha != 255) {
            for (std::size_t c = 0; c < 3; ++c) {
                premultiplied[i + c] =
                    static_cast<std::uint8_t>(((premultiplied[i + c] * alpha) + 127U) / 255U);
            }
        }
    }
    glGenTextures(1, &id_);
    glBindTexture(GL_TEXTURE_2D, id_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
                 static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, premultiplied.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

Texture::~Texture() { glDeleteTextures(1, &id_); }

std::size_t Texture::bytes() const noexcept { return std::size_t{width_} * height_ * 4 * 4 / 3; }

}  // namespace rummage::ui
