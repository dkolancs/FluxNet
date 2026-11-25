#pragma once

#include "fluxnet/config.hpp"
#include "fluxnet/connection.hpp"
#include "fluxnet/metrics.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace fluxnet {

// TCP server. run() blocks on the acceptor thread until stop().
// Configure callbacks only before run(). Destroy only after run() has returned,
// or from a thread that is not inside a callback (the destructor waits for Stopped).
class Server {
  public:
    explicit Server(ServerConfig config);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void set_on_connect(ConnectCallback cb);
    void set_on_message(MessageCallback cb);
    void set_on_disconnect(DisconnectCallback cb);
    void set_on_error(ErrorCallback cb);
    void set_on_backpressure(BackpressureCallback cb);

    void run();
    void stop() noexcept;
    bool wait_until_running(std::chrono::milliseconds timeout) const;

    ServerState state() const noexcept;
    std::uint16_t bound_port() const noexcept;
    IoBackend active_backend() const noexcept;
    // Empty when the requested backend was used. Otherwise the fallback reason.
    std::string backend_note() const;
    MetricsSnapshot metrics() const;

    // Admission finishes before either call returns. Accepted means the frame is
    // queued and will be written, or the connection will close or error. It is
    // not a silent drop. Backpressured means nothing from that call was queued.
    // Neither call waits for the kernel write or for another worker's loop.
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
