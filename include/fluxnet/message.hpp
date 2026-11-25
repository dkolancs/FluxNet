#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace fluxnet {

enum class MessageType : std::uint16_t {
    Data = 1,
    Ping = 2,
    Pong = 3,
    Error = 4,
    Close = 5,
};

enum class SendResult {
    Accepted,
    Backpressured,
    Closed,
    InvalidMessage,
};

// Points at decoder memory. Valid only until on_message returns.
// The callback may copy the bytes. It must not store this view.
class MessageView {
  public:
    MessageView() = default;
    MessageView(MessageType type, std::uint16_t flags, const std::byte* data,
                std::size_t size) noexcept
        : type_(type), flags_(flags), data_(data), size_(size) {}

    MessageType type() const noexcept { return type_; }
    std::uint16_t flags() const noexcept { return flags_; }
    std::span<const std::byte> payload() const noexcept { return {data_, size_}; }
    bool empty() const noexcept { return size_ == 0; }

  private:
    MessageType type_ = MessageType::Data;
    std::uint16_t flags_ = 0;
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
};

const char* to_string(SendResult result) noexcept;
bool valid_message_type(std::uint16_t raw) noexcept;

} // namespace fluxnet
