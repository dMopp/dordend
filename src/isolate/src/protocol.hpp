#pragma once

// What rummage and its preview helper say to each other, on a private SOCK_SEQPACKET socket
// (one message per packet). rummage sends a request with the file's descriptor; the helper
// answers a picture with the result and a sealed memfd holding its pixels, a text with the result
// and a sealed memfd holding its lines, or the information as text fields. A helper leaving after
// its idle time first sends a goodbye, so that a request racing with its exit is sent again to a
// new one. This is the protocol engines' manifests name (engines::kProtocol): a change to these
// messages raises both.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rummage/picture/photo_info.hpp"
#include "rummage/text/text.hpp"

namespace rummage::isolate::protocol {

// The descriptor the helper finds its socket on.
inline constexpr int kSocketFd = 3;

inline constexpr std::uint32_t kRequestMagic = 0x4E505251;   // "NPRQ"
inline constexpr std::uint32_t kResponseMagic = 0x4E505253;  // "NPRS"
inline constexpr std::uint32_t kGoodbyeMagic = 0x4E504259;   // "NPBY"
inline constexpr std::uint32_t kInfoMagic = 0x4E50494E;      // "NPIN"

inline constexpr std::size_t kMaxDetail = 1024;
inline constexpr std::size_t kMaxPacket = 8192;
inline constexpr std::size_t kMaxField = 255;  // bytes of one information field, at most

// The helper's exit status when it could not build its cage: it never works without one.
inline constexpr int kSandboxFailedExit = 3;

// What rummage asks for.
// Four bytes, for a request laid out without padding.
enum class Kind : std::uint32_t {  // NOLINT(performance-enum-size)
    picture = 0,                   // the file's picture, max_width x max_height at most
    photo_info = 1,                // what the file says about itself (EXIF)
    text = 2,                      // the file's text (TextRequest)
};

struct Request {
    std::uint32_t magic{kRequestMagic};
    Kind kind{Kind::picture};
    std::uint32_t max_width{0};
    std::uint32_t max_height{0};
};

struct Response {
    std::uint32_t magic{kResponseMagic};
    std::uint8_t failure{0};  // picture::Failure
    std::uint8_t from_preview{0};
    std::uint16_t detail_length{0};  // bytes of detail after this header, at most kMaxDetail
    std::uint32_t width{0};          // the picture in the memfd: width x height RGBA
    std::uint32_t height{0};
    std::uint32_t full_width{0};
    std::uint32_t full_height{0};
};

inline constexpr std::uint32_t kTextMagic = 0x4E505458;  // "NPTX"

// What a text request may ask for, at most: a helper refuses more.
inline constexpr std::size_t kMaxTextBytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t kMaxTextLineBytes = std::size_t{64} * 1024;
inline constexpr std::size_t kMaxTabWidth = 16;

// A text request: this, then the file's name and its MIME type (name_length and mime_length
// bytes), for the engine to choose a language by (ROADMAP 3b-2). Never the file's path.
struct TextRequest {
    std::uint32_t magic{kRequestMagic};
    Kind kind{Kind::text};
    std::uint32_t max_bytes{0};
    std::uint32_t max_line_bytes{0};
    std::uint32_t max_lines{0};  // 0: no limit
    std::uint16_t tab_width{0};
    std::uint8_t name_length{0};
    std::uint8_t mime_length{0};
    std::uint8_t tail{0};  // 1: the file's end (text::Limits::tail)
};

// A text answer: this, then detail_length bytes of detail and language_length bytes of the
// language's name; with failure none and something to send, a sealed memfd holding the payload
// (encode_text_payload).
struct TextResponse {
    std::uint32_t magic{kTextMagic};
    std::uint8_t failure{0};   // text::Failure: none, binary or unreadable
    std::uint8_t encoding{0};  // text::Encoding
    std::uint8_t truncated{0};
    std::uint8_t crlf{0};
    std::uint32_t lines{0};
    std::uint32_t bytes{0};          // of the lines joined by '\n'
    std::uint32_t spans{0};          // coloured pieces, all lines together
    std::uint16_t detail_length{0};  // at most kMaxDetail
    std::uint8_t language_length{0};
    std::uint8_t tail{0};  // 1: the lines are the file's end (text::Text::tail)
};

// A text's payload: its lines joined by '\n' (bytes), zeros to a multiple of 4, then each span
// as three 32-bit numbers: its line, its start, its length << 8 | its style. Spans come in
// order of lines, then of starts.
[[nodiscard]] std::vector<std::byte> encode_text_payload(const text::Text& text,
                                                         std::uint32_t& bytes,
                                                         std::uint32_t& spans);

// The payload's size for bytes and spans.
[[nodiscard]] std::size_t text_payload_size(std::uint32_t bytes, std::uint32_t spans);

// Lines and spans back from a payload (text's lines and spans set; its spans one per line when
// spans > 0 or coloured is true); false unless every line is within limits.max_line_bytes and
// every span is inside its line, after the one before, of a known style.
[[nodiscard]] bool decode_text_payload(std::span<const std::byte> payload, std::uint32_t lines,
                                       std::uint32_t bytes, std::uint32_t spans, bool coloured,
                                       const text::Limits& limits, text::Text& text);

// What a text request asks for.
struct TextAsk {
    text::Limits limits;
    std::string name;
    std::string mime;
};

// A text request as one packet (name and mime cut at kMaxField bytes), and back; nullopt when the
// bytes are not one or ask for more than the limits above (a zero limit included).
[[nodiscard]] std::vector<std::byte> encode_text_request(const text::Limits& limits,
                                                         std::string_view name,
                                                         std::string_view mime);
[[nodiscard]] std::optional<TextAsk> decode_text_request(std::span<const std::byte> bytes);

inline constexpr std::size_t kMaxFds = 2;

// A packet received: its bytes and the descriptors that came with it (-1 where none). The
// descriptors are the receiver's to close (close_fds).
struct Packet {
    std::array<std::byte, kMaxPacket> bytes{};
    std::size_t length{0};
    std::array<int, kMaxFds> fds{-1, -1};

    void close_fds();
};

// Sends one packet with up to kMaxFds descriptors. False on an error (errno set); never raises
// SIGPIPE.
[[nodiscard]] bool send_packet(int socket, std::span<const std::byte> bytes,
                               std::span<const int> fds = {});

// Receives one packet; nullopt at the end of the stream (the other side closed) or on an error
// (errno set; 0 at the end). Descriptors beyond kMaxFds are closed.
[[nodiscard]] std::optional<Packet> receive_packet(int socket);

// A photo's information as one packet (kInfoMagic, the numbers, then each text field, cut at
// kMaxField bytes), and back; nullopt when the bytes are not that.
[[nodiscard]] std::vector<std::byte> encode_info(const picture::PhotoInfo& info);
[[nodiscard]] std::optional<picture::PhotoInfo> decode_info(std::span<const std::byte> bytes);

}  // namespace rummage::isolate::protocol
