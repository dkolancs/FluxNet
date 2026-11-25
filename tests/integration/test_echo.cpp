#include "fluxnet/client.hpp"
#include "fluxnet/server.hpp"
#include "support/wait.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace {

struct Running {
    fluxnet::Server server;
    std::thread thread;
    explicit Running(fluxnet::ServerConfig config) : server(std::move(config)) {}
    void start() {
        thread = std::thread([this] { server.run(); });
        ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    }
    ~Running() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

struct RunningClient {
    fluxnet::Client client;
    std::thread thread;
    explicit RunningClient(fluxnet::ClientConfig config) : client(std::move(config)) {}
    void start() {
        thread = std::thread([this] { client.run(); });
        ASSERT_TRUE(client.wait_until_running(std::chrono::seconds(2)));
    }
    ~RunningClient() {
        client.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
};

fluxnet::ServerConfig echo_config(int workers, fluxnet::EpollTrigger trigger) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = workers;
    config.epoll_trigger = trigger;
    config.io_backend = fluxnet::IoBackend::Epoll;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    return config;
}

} // namespace

TEST(Echo, RoundTripLevelAndEdge) {
    for (auto trigger : {fluxnet::EpollTrigger::Level, fluxnet::EpollTrigger::Edge}) {
        auto config = echo_config(2, trigger);
        Running running(config);
        running.server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView msg) {
            EXPECT_EQ(conn.send(msg.type(), msg.payload(), msg.flags()),
                      fluxnet::SendResult::Accepted);
        });
        running.start();

        fluxnet::ClientConfig client_config;
        client_config.host = "127.0.0.1";
        client_config.port = running.server.bound_port();
        client_config.io_backend = fluxnet::IoBackend::Epoll;
        client_config.idle_timeout = std::chrono::milliseconds(0);
        client_config.slow_consumer_timeout = std::chrono::milliseconds(0);
        fluxnet::test::Gate gate;
        std::vector<std::byte> got;
        RunningClient client(client_config);
        client.client.set_on_message([&](fluxnet::Connection&, fluxnet::MessageView msg) {
            got.assign(msg.payload().begin(), msg.payload().end());
            gate.notify();
        });
        client.start();
        auto id = client.client.connect();
        ASSERT_TRUE(id.valid());
        std::vector<std::byte> payload{std::byte{'p'}, std::byte{'i'}, std::byte{'n'},
                                       std::byte{'g'}};
        ASSERT_EQ(client.client.send(id, fluxnet::MessageType::Data, payload, 9),
                  fluxnet::SendResult::Accepted);
        ASSERT_TRUE(gate.wait_for(1, std::chrono::seconds(2)));
        EXPECT_EQ(got, payload);
    }
}

TEST(Echo, CallbackExceptionDoesNotKillServer) {
    auto config = echo_config(1, fluxnet::EpollTrigger::Level);
    Running running(config);
    std::atomic<int> connects{0};
    running.server.set_on_connect([&](fluxnet::Connection&) { connects.fetch_add(1); });
    running.server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView) {
        throw std::runtime_error("boom");
        (void)conn;
    });
    running.start();

    fluxnet::ClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = running.server.bound_port();
    client_config.io_backend = fluxnet::IoBackend::Epoll;
    fluxnet::test::Gate closed;
    RunningClient client(client_config);
    client.client.set_on_disconnect([&](fluxnet::Connection&) { closed.notify(); });
    client.start();
    auto id = client.client.connect();
    ASSERT_TRUE(id.valid());
    std::vector<std::byte> payload{std::byte{1}};
    client.client.send(id, fluxnet::MessageType::Data, payload, 0);
    ASSERT_TRUE(closed.wait_for(1, std::chrono::seconds(2)));
    EXPECT_GE(connects.load(), 1);
    // The server is still in run(); a callback exception must not stop it.
    EXPECT_EQ(running.server.state(), fluxnet::ServerState::Running);
    EXPECT_GE(running.server.metrics().callback_exceptions, 1u);
}

TEST(Echo, ManyClients) {
    auto config = echo_config(2, fluxnet::EpollTrigger::Level);
    Running running(config);
    running.server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        conn.send(msg.type(), msg.payload(), msg.flags());
    });
    running.start();
    constexpr int kClients = 20;
    std::atomic<int> got{0};
    std::vector<std::unique_ptr<fluxnet::Client>> clients;
    std::vector<std::thread> runners;
    for (int i = 0; i < kClients; ++i) {
        fluxnet::ClientConfig client_config;
        client_config.host = "127.0.0.1";
        client_config.port = running.server.bound_port();
        client_config.io_backend = fluxnet::IoBackend::Epoll;
        auto client = std::make_unique<fluxnet::Client>(client_config);
        client->set_on_message([&, i](fluxnet::Connection&, fluxnet::MessageView msg) {
            if (!msg.payload().empty() && std::to_integer<int>(msg.payload()[0]) == i) {
                got.fetch_add(1);
            }
        });
        runners.emplace_back([c = client.get()] { c->run(); });
        ASSERT_TRUE(client->wait_until_running(std::chrono::seconds(2)));
        clients.push_back(std::move(client));
    }
    std::vector<fluxnet::ConnectionId> ids;
    for (auto& client : clients) {
        ids.push_back(client->connect());
        ASSERT_TRUE(ids.back().valid());
    }
    for (int i = 0; i < kClients; ++i) {
        std::vector<std::byte> payload{static_cast<std::byte>(i)};
        EXPECT_EQ(clients[static_cast<std::size_t>(i)]->send(
                      ids[static_cast<std::size_t>(i)], fluxnet::MessageType::Data, payload, 0),
                  fluxnet::SendResult::Accepted);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (got.load() < kClients && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(got.load(), kClients);
    for (auto& client : clients) {
        client->stop();
    }
    for (auto& thread : runners) {
        thread.join();
    }
}

TEST(Echo, IoUringIfAvailable) {
    fluxnet::ServerConfig config = echo_config(1, fluxnet::EpollTrigger::Level);
    config.io_backend = fluxnet::IoBackend::IoUring;
    Running running(config);
    running.server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        conn.send(msg.type(), msg.payload(), msg.flags());
    });
    running.start();
    if (running.server.active_backend() != fluxnet::IoBackend::IoUring) {
        GTEST_SKIP() << running.server.backend_note();
    }
    EXPECT_TRUE(running.server.backend_note().empty());
    fluxnet::ClientConfig client_config;
    client_config.host = "127.0.0.1";
    client_config.port = running.server.bound_port();
    client_config.io_backend = fluxnet::IoBackend::IoUring;
    fluxnet::test::Gate gate;
    RunningClient client(client_config);
    client.client.set_on_message(
        [&](fluxnet::Connection&, fluxnet::MessageView) { gate.notify(); });
    client.start();
    if (client.client.active_backend() != fluxnet::IoBackend::IoUring) {
        GTEST_SKIP() << client.client.backend_note();
    }
    auto id = client.client.connect();
    ASSERT_TRUE(id.valid());
    std::vector<std::byte> payload{std::byte{'u'}};
    ASSERT_EQ(client.client.send(id, fluxnet::MessageType::Data, payload, 0),
              fluxnet::SendResult::Accepted);
    EXPECT_TRUE(gate.wait_for(1, std::chrono::seconds(2))) << running.server.backend_note();
}
