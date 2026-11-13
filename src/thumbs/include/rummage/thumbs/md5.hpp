#pragma once

#include <string>
#include <string_view>

namespace rummage::thumbs {

// MD5 (RFC 1321) of data as 32 lowercase hex digits: how the freedesktop thumbnail spec names
// the thumbnail of a URI. Not for anything that needs security.
[[nodiscard]] std::string md5_hex(std::string_view data);

}  // namespace rummage::thumbs
