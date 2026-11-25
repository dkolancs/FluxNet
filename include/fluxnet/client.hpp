#pragma once

#include "fluxnet/config.hpp"
#include "fluxnet/connection.hpp"
#include "fluxnet/metrics.hpp"

#include <chrono>
#include <memory>
#include <span>

namespace fluxnet {

// Outbound connections. run() blocks until stop(). connect() is safe from another thread
// once wait_until_running() is true. An invalid ConnectionId means connect failed.
class Client {
  public:
    explicit Client(ClientConfig config);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    void set_on_connect(ConnectCallback cb);
    void set_on_message(MessageCallback cb);
    void set_on_disconnect(DisconnectCallback cb);
    void set_on_error(ErrorCallback cb);
    void set_on_backpressure(BackpressureCallback cb);

    void run();
    void stop() noexcept;
    bool wait_until_running(std::chrono::milliseconds timeout) const;

    ServerState state() const noexcept;
    IoBackend active_backend() const noexcept;
    std::string backend_note() const;
    MetricsSnapshot metrics() const;

    ConnectionId connect();
    ConnectionId connect_for(std::chrono::milliseconds timeout);

    SendResult send(ConnectionId id, MessageType type, std::span<const std::byte> payload,
                    std::uint16_t flags = 0);
    SendResult try_send(ConnectionId id, MessageType type, std::span<const std::byte> payload,
                        std::uint16_t flags = 0);
    PostResult post(ConnectionId id, std::function<void(Connection&)> fn);
    void close(ConnectionId id);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fluxnet
