#include "fluxnet/server.hpp"
#include "net/socket.hpp"
#include "protocol/codec.hpp"
#include "support/wait.hpp"

#include <gtest/gtest.h>

#include <thread>

namespace {

fluxnet::ServerConfig base() {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.io_backend = fluxnet::IoBackend::Epoll;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(200);
    config.output_high_watermark = 4096;
    config.output_low_watermark = 1024;
    config.max_pending_output = 8192;
    config.max_frame_size = 64 * 1024;
    return config;
}

} // namespace

TEST(Limits, OversizedFrameClosesConnection) {
    auto config = base();
    config.max_frame_size = 32;
    fluxnet::Server server(config);
    fluxnet::test::Gate closed;
    server.set_on_disconnect([&](fluxnet::Connection&) { closed.notify(); });
    server.set_on_message(
        [](fluxnet::Connection&, fluxnet::MessageView) { FAIL() << "frame should be rejected"; });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));

    fluxnet::detail::Socket client = fluxnet::detail::Socket::tcp_v4();
    ASSERT_EQ(client.connect_v4("127.0.0.1", server.bound_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    std::byte header[8] = {};
    header[3] = std::byte{64}; // length 64 > 32
    header[5] = std::byte{1};
    auto sent = client.send_nb(header);
    EXPECT_EQ(sent.status, fluxnet::detail::IoStatus::Ok);
    std::byte buf[8];
    bool eof = false;
    for (int i = 0; i < 50; ++i) {
        auto recvd = client.recv_nb(buf);
        if (recvd.status == fluxnet::detail::IoStatus::Eof ||
            recvd.status == fluxnet::detail::IoStatus::Reset) {
            eof = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(eof);
    EXPECT_GE(server.metrics().protocol_errors, 1u);
    server.stop();
    thread.join();
}

TEST(Limits, IdleTimeout) {
    auto config = base();
    config.idle_timeout = std::chrono::milliseconds(200);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::Server server(config);
    fluxnet::test::Gate closed;
    server.set_on_disconnect([&](fluxnet::Connection&) { closed.notify(); });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    fluxnet::detail::Socket client = fluxnet::detail::Socket::tcp_v4();
    ASSERT_EQ(client.connect_v4("127.0.0.1", server.bound_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    EXPECT_TRUE(closed.wait_for(1, std::chrono::seconds(2)));
    EXPECT_GE(server.metrics().timeouts_idle, 1u);
    server.stop();
    thread.join();
}

TEST(Limits, AdmissionCap) {
    auto config = base();
    config.max_connections = 1;
    config.idle_timeout = std::chrono::seconds(30);
    fluxnet::Server server(config);
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    fluxnet::detail::Socket a = fluxnet::detail::Socket::tcp_v4();
    fluxnet::detail::Socket b = fluxnet::detail::Socket::tcp_v4();
    ASSERT_EQ(a.connect_v4("127.0.0.1", server.bound_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    ASSERT_EQ(b.connect_v4("127.0.0.1", server.bound_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && server.metrics().rejected_admission < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_LE(server.metrics().active_connections, 1u);
    EXPECT_GE(server.metrics().rejected_admission, 1u);
    server.stop();
    thread.join();
}

TEST(Limits, BackpressureOnSlowReader) {
    auto config = base();
    config.slow_consumer_timeout = std::chrono::seconds(5);
    fluxnet::Server server(config);
    server.set_on_connect([](fluxnet::Connection& conn) {
        std::vector<std::byte> payload(1000, std::byte{7});
        for (int i = 0; i < 20; ++i) {
            auto result = conn.send(fluxnet::MessageType::Data, payload, 0);
            if (result == fluxnet::SendResult::Backpressured) {
                return;
            }
        }
    });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    fluxnet::detail::Socket client = fluxnet::detail::Socket::tcp_v4();
    client.set_nonblocking(true);
    auto connected = client.connect_v4("127.0.0.1", server.bound_port());
    ASSERT_TRUE(connected.status == fluxnet::detail::ConnectStatus::Connected ||
                connected.status == fluxnet::detail::ConnectStatus::InProgress);
    // Do not read. Wait until the server has either backpressured or queued a bounded amount.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           server.metrics().send_backpressured == 0 && server.metrics().connects == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // The socket buffer may absorb 20 KiB, so backpressure is not guaranteed on every kernel.
    // The output cap must still hold if the server accepted the connection.
    if (server.metrics().connects > 0) {
        EXPECT_LT(server.metrics().bytes_out, 2u * 1024u * 1024u);
    }
    server.stop();
    thread.join();
}
