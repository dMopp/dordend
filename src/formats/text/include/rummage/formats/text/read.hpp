#pragma once

#include <cstdint>
#include <span>

#include "rummage/text/text.hpp"

// The text family (ROADMAP 3b): text files read for Quick Look and the info pane, in the base
// engine. The encoding is found from the bytes: a UTF-8 or UTF-16 byte order mark, else UTF-8
// when the bytes are valid UTF-8, else Windows-1252 (Latin-1's superset). Linked into the
// isolated helper (and tests and tools), never into rummage.
namespace rummage::formats::text {

// The text in bytes, the beginning of a file that goes on when more is true. Binary when a NUL
// shows up; never unreadable or unavailable. Lines end at "\n" or "\r\n"; a last line cut by the
// limit is left out unless it is the only one.
[[nodiscard]] rummage::text::Loaded decode(std::span<const std::uint8_t> bytes, bool more,
                                           const rummage::text::Limits& limits);

// The text of the file open as fd (the caller's, kept open), read from its start with pread; with
// limits.tail, from its end when it is longer than limits.max_bytes (read_tail).
[[nodiscard]] rummage::text::Loaded read(int fd, const rummage::text::Limits& limits);

// The end of the file open as fd, size bytes long (more than limits.max_bytes): its last
// max_bytes from the first line starting in them; truncated and tail set.
[[nodiscard]] rummage::text::Loaded read_tail(int fd, std::size_t size,
                                              const rummage::text::Limits& limits);

// The text of request.path, read in this process (tests and tools), coloured as the language its
// name, type or first line says (syntax.hpp).
[[nodiscard]] rummage::text::Loaded load(const rummage::text::LoadRequest& request);

}  // namespace rummage::formats::text
