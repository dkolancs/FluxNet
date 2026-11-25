#pragma once

#include "fluxnet/message.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace fluxnet::detail {

enum class DecodeError {
    Ok = 0,
    FrameTooLarge,
    BadType,
    InputTooLarge,
};

inline void write_be32(std::byte* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::byte>((v >> 24) & 0xffu);
    p[1] = static_cast<std::byte>((v >> 16) & 0xffu);
    p[2] = static_cast<std::byte>((v >> 8) & 0xffu);
    p[3] = static_cast<std::byte>(v & 0xffu);
}

inline void write_be16(std::byte* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::byte>((v >> 8) & 0xffu);
    p[1] = static_cast<std::byte>(v & 0xffu);
}

inline std::uint32_t read_be32(const std::byte* p) noexcept {
    return (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[0])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[2])) << 8) |
           static_cast<std::uint32_t>(std::to_integer<unsigned char>(p[3]));
}

inline std::uint16_t read_be16(const std::byte* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<unsigned>(std::to_integer<unsigned char>(p[0])) << 8) |
        static_cast<unsigned>(std::to_integer<unsigned char>(p[1])));
}

// Writes an 8-byte header and payload into dest. Returns false if the payload
// exceeds max_frame or dest is too small. Does not allocate.
bool write_frame(std::byte* dest, std::size_t cap, MessageType type, std::uint16_t flags,
                 std::span<const std::byte> payload, std::size_t max_frame,
                 std::size_t& written) noexcept;

// Incremental decoder. Length is checked against max_frame before payload allocation.
// feed() invokes on_msg(MessageView) -> bool. Return false from the callback to stop;
// remaining bytes in this buffer are discarded (used when the connection is closing).
// The view is valid only for the duration of that callback.
class FrameDecoder {
  public:
    FrameDecoder(std::size_t max_frame, std::size_t max_input)
        : max_frame_(max_frame), max_input_(max_input) {}

    template <typename Fn> bool feed(const std::byte* data, std::size_t n, Fn&& on_msg) {
        std::size_t off = 0;
        while (off < n) {
            if (!in_payload_) {
                const std::size_t need = 8 - hdr_have_;
                const std::size_t take = std::min(need, n - off);
                for (std::size_t i = 0; i < take; ++i) {
                    hdr_[hdr_have_ + i] = data[off + i];
                }
                hdr_have_ += take;
                off += take;
                if (hdr_have_ < 8) {
                    return true;
                }
                const std::uint32_t len = read_be32(hdr_.data());
                const std::uint16_t type = read_be16(hdr_.data() + 4);
                const std::uint16_t flags = read_be16(hdr_.data() + 6);
                if (static_cast<std::size_t>(len) > max_frame_) {
                    error_ = DecodeError::FrameTooLarge;
                    return false;
                }
                if (!valid_message_type(type)) {
                    error_ = DecodeError::BadType;
                    return false;
                }
                if (buffered_estimate(len) > max_input_) {
                    error_ = DecodeError::InputTooLarge;
                    return false;
                }
                len_ = len;
                type_ = static_cast<MessageType>(type);
                flags_ = flags;
                payload_.clear();
                payload_.resize(len_);
                payload_have_ = 0;
                in_payload_ = true;
            }
            const std::size_t need = static_cast<std::size_t>(len_) - payload_have_;
            const std::size_t take = std::min(need, n - off);
            if (take > 0 && !payload_.empty()) {
                std::memcpy(payload_.data() + payload_have_, data + off, take);
            }
            payload_have_ += take;
            off += take;
            if (payload_have_ < len_) {
                return true;
            }
            MessageView view(type_, flags_, payload_.empty() ? nullptr : payload_.data(),
                             payload_.size());
            const bool keep = on_msg(view);
            hdr_have_ = 0;
            in_payload_ = false;
            len_ = 0;
            payload_have_ = 0;
            if (!keep) {
                return true;
            }
        }
        return true;
    }

    DecodeError error() const noexcept { return error_; }
    std::size_t buffered() const noexcept { return hdr_have_ + (in_payload_ ? payload_have_ : 0); }
    void reset() noexcept {
        hdr_have_ = 0;
        in_payload_ = false;
        len_ = 0;
        payload_have_ = 0;
        payload_.clear();
        error_ = DecodeError::Ok;
    }

  private:
    std::size_t buffered_estimate(std::uint32_t new_len) const noexcept {
        return 8u + static_cast<std::size_t>(new_len);
    }

    std::size_t max_frame_ = 0;
    std::size_t max_input_ = 0;
    std::array<std::byte, 8> hdr_{};
    std::size_t hdr_have_ = 0;
    bool in_payload_ = false;
    std::uint32_t len_ = 0;
    MessageType type_ = MessageType::Data;
    std::uint16_t flags_ = 0;
    std::vector<std::byte> payload_;
    std::size_t payload_have_ = 0;
    DecodeError error_ = DecodeError::Ok;
};

} // namespace fluxnet::detail
