#pragma once

#include "fluxnet/message.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace fluxnet {

namespace detail {
class Conn;
class Worker;
} // namespace detail

struct ConnectionId {
    std::uint32_t worker = 0;
    std::uint32_t index = 0;
    std::uint32_t generation = 0;

    constexpr bool valid() const noexcept { return generation != 0; }
    friend constexpr bool operator==(ConnectionId a, ConnectionId b) noexcept {
        return a.worker == b.worker && a.index == b.index && a.generation == b.generation;
    }
    friend constexpr bool operator!=(ConnectionId a, ConnectionId b) noexcept { return !(a == b); }
};

// Non-owning view of a connection owned by one worker.
// Safe only on that worker, during a callback or a task the worker is running.
// send() and close() are owner-thread operations.
class Connection {
  public:
    ConnectionId id() const noexcept;
    // Peer "a.b.c.d:port" captured at accept/connect. Valid while the view is.
    const std::string& peer() const noexcept;
    SendResult send(MessageType type, std::span<const std::byte> payload, std::uint16_t flags = 0);
    void close();
    bool is_open() const noexcept;

  private:
    friend class detail::Worker;
    friend class Server;
    friend class Client;
    explicit Connection(detail::Conn* conn) noexcept : conn_(conn) {}
    detail::Conn* conn_ = nullptr;
};

using ConnectCallback = std::function<void(Connection&)>;
using MessageCallback = std::function<void(Connection&, MessageView)>;
using DisconnectCallback = std::function<void(Connection&)>;
using ErrorCallback = std::function<void(Connection&, int err, std::string_view what)>;
using BackpressureCallback = std::function<void(Connection&, bool paused)>;

enum class PostResult {
    Queued,
    Full,
    Closed,
    Invalid,
};

} // namespace fluxnet

namespace std {
template <> struct hash<fluxnet::ConnectionId> {
    std::size_t operator()(fluxnet::ConnectionId id) const noexcept {
        return (static_cast<std::size_t>(id.worker) * 1315423911u) ^
               (static_cast<std::size_t>(id.index) << 16) ^ static_cast<std::size_t>(id.generation);
    }
};
} // namespace std
