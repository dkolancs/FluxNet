#include "protocol/codec.hpp"

#include <cstring>

namespace fluxnet::detail {

bool write_frame(std::byte* dest, std::size_t cap, MessageType type, std::uint16_t flags,
                 std::span<const std::byte> payload, std::size_t max_frame,
                 std::size_t& written) noexcept {
    written = 0;
    if (payload.size() > max_frame) {
        return false;
    }
    if (!valid_message_type(static_cast<std::uint16_t>(type))) {
        return false;
    }
    const std::size_t total = 8 + payload.size();
    if (cap < total || dest == nullptr) {
        return false;
    }
    write_be32(dest, static_cast<std::uint32_t>(payload.size()));
    write_be16(dest + 4, static_cast<std::uint16_t>(type));
    write_be16(dest + 6, flags);
    if (!payload.empty()) {
        std::memcpy(dest + 8, payload.data(), payload.size());
    }
    written = total;
    return true;
}

} // namespace fluxnet::detail
