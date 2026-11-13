#include "internal.hpp"
#include "rummage/picture/wuffs.hpp"

namespace rummage::svg {
std::shared_ptr<picture::Image> embedded(Document& doc, std::string_view href) {
    if (!href.starts_with("data:image/png;base64,") && !href.starts_with("data:image/jpeg;base64,"))
        return {};
    doc.charge_resource(href.size());
    const bool png = href.starts_with("data:image/png;");
    href.remove_prefix(href.find(',') + 1);
    std::vector<std::uint8_t> data;
    data.reserve(href.size() / 4 * 3);
    unsigned accumulator = 0, bits = 0, padding = 0;
    std::size_t digits = 0;
    for (char c : href) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        if (c == '=') {
            require(++padding <= 2);
            continue;
        }
        require(padding == 0);
        unsigned v = 0;
        if (c >= 'A' && c <= 'Z')
            v = static_cast<unsigned>(c - 'A');
        else if (c >= 'a' && c <= 'z')
            v = static_cast<unsigned>(c - 'a') + 26;
        else if (c >= '0' && c <= '9')
            v = static_cast<unsigned>(c - '0') + 52;
        else if (c == '+')
            v = 62;
        else if (c == '/')
            v = 63;
        else
            throw Failure{};
        accumulator = (accumulator << 6) | v;
        bits += 6;
        ++digits;
        if (bits >= 8) {
            bits -= 8;
            data.push_back(static_cast<std::uint8_t>(accumulator >> bits));
        }
    }
    require((digits + padding) % 4 == 0 && bits == padding * 2 &&
            (accumulator & ((1U << bits) - 1)) == 0);
    if (png) {
        constexpr std::array<std::uint8_t, 8> signature = {137, 80, 78, 71, 13, 10, 26, 10};
        require(data.size() >= signature.size() &&
                std::equal(signature.begin(), signature.end(), data.begin()));
    } else
        require(data.size() >= 3 && data[0] == 255 && data[1] == 216 && data[2] == 255);
    auto decoded = picture::decode_wuffs(data, 1024U * 1024U);
    require(!decoded.image.empty());
    doc.texture_bytes += decoded.image.rgba.size();
    require(doc.texture_bytes + doc.layer_bytes <= 64U * 1024U * 1024U);
    for (std::size_t i = 0; i < decoded.image.rgba.size(); i += 4)
        for (std::size_t c = 0; c < 3; ++c)
            decoded.image.rgba[i + c] = static_cast<std::uint8_t>(
                (decoded.image.rgba[i + c] * decoded.image.rgba[i + 3] + 127) / 255);
    return std::make_shared<picture::Image>(std::move(decoded.image));
}
}  // namespace rummage::svg
