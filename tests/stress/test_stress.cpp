#include "fluxnet/client.hpp"
#include "fluxnet/server.hpp"
#include "support/wait.hpp"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

namespace {

fluxnet::ServerConfig config() {
    fluxnet::ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.worker_threads = 1;
    cfg.io_backend = fluxnet::IoBackend::Epoll;
    cfg.idle_timeout = std::chrono::milliseconds(0);
    cfg.slow_consumer_timeout = std::chrono::milliseconds(0);
    cfg.shutdown_timeout = std::chrono::milliseconds(300);
    return cfg;
}

} // namespace

TEST(Stress, RepeatedStartStop) {
    for (int i = 0; i < 8; ++i) {
        fluxnet::Server server(config());
        std::thread thread([&] { server.run(); });
        ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2))) << i;
        server.stop();
        thread.join();
        EXPECT_EQ(server.state(), fluxnet::ServerState::Stopped);
    }
}

TEST(Stress, ConnectionChurn) {
    fluxnet::Server server(config());
    server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        conn.send(msg.type(), msg.payload(), msg.flags());
    });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));

    fluxnet::ClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = server.bound_port();
    client_config.io_backend = fluxnet::IoBackend::Epoll;
    client_config.idle_timeout = std::chrono::milliseconds(0);
    client_config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::test::Gate gate;
    fluxnet::Client client(client_config);
    client.set_on_message([&](fluxnet::Connection&, fluxnet::MessageView) { gate.notify(); });
    std::thread client_thread([&] { client.run(); });
    ASSERT_TRUE(client.wait_until_running(std::chrono::seconds(2)));
    std::vector<std::byte> payload{std::byte{1}};
    for (int i = 0; i < 20; ++i) {
        auto id = client.connect();
        ASSERT_TRUE(id.valid()) << i;
        ASSERT_EQ(client.send(id, fluxnet::MessageType::Data, payload, 0),
                  fluxnet::SendResult::Accepted);
        ASSERT_TRUE(gate.wait_for(i + 1, std::chrono::seconds(2))) << i;
        client.close(id);
    }
    client.stop();
    client_thread.join();
    server.stop();
    thread.join();
}

TEST(Stress, SlowReaderStaysBounded) {
    auto cfg = config();
    cfg.max_pending_output = 4096;
    cfg.output_high_watermark = 2048;
    cfg.output_low_watermark = 512;
    fluxnet::Server server(cfg);
    server.set_on_connect([&](fluxnet::Connection& conn) {
        std::vector<std::byte> payload(512, std::byte{9});
        for (int i = 0; i < 32; ++i) {
            if (conn.send(fluxnet::MessageType::Data, payload, 0) ==
                fluxnet::SendResult::Backpressured) {
                break;
            }
        }
    });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    fluxnet::ClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = server.bound_port();
    client_config.idle_timeout = std::chrono::milliseconds(0);
    client_config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::Client client(client_config);
    std::thread client_thread([&] { client.run(); });
    ASSERT_TRUE(client.wait_until_running(std::chrono::seconds(2)));
    auto id = client.connect();
    ASSERT_TRUE(id.valid());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && server.metrics().connects == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(server.metrics().connects, 1u);
    EXPECT_LT(server.metrics().bytes_out, 2u * 1024u * 1024u);
    client.stop();
    client_thread.join();
    server.stop();
    thread.join();
}
