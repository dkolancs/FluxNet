#include "fluxnet/client.hpp"
#include "fluxnet/server.hpp"
#include "kv/store.hpp"
#include "pubsub/broker.hpp"
#include "support/wait.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <thread>
#include <vector>

namespace {

class ServerThread {
  public:
    explicit ServerThread(fluxnet::ServerConfig config) : server(std::move(config)) {}
    void start() {
        thread = std::thread([this] { server.run(); });
        ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    }
    ~ServerThread() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
    fluxnet::Server server;
    std::thread thread;
};

class ClientThread {
  public:
    explicit ClientThread(fluxnet::ClientConfig config) : client(std::move(config)) {}
    void start() {
        thread = std::thread([this] { client.run(); });
        ASSERT_TRUE(client.wait_until_running(std::chrono::seconds(2)));
    }
    ~ClientThread() {
        client.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }
    fluxnet::Client client;
    std::thread thread;
};

fluxnet::ClientConfig client_cfg(std::uint16_t port) {
    fluxnet::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.io_backend = fluxnet::IoBackend::Epoll;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    return config;
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(text[i]);
    }
    return out;
}

std::string text_of(fluxnet::MessageView msg) {
    std::string text;
    text.resize(msg.payload().size());
    for (std::size_t i = 0; i < msg.payload().size(); ++i) {
        text[i] = static_cast<char>(msg.payload()[i]);
    }
    return text;
}

} // namespace

TEST(PubSub, FanoutAndUnsubscribe) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 2;
    config.io_backend = fluxnet::IoBackend::Epoll;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    ServerThread running(config);
    fluxnet::examples::Broker broker(running.server);
    running.server.set_on_message(
        [&](fluxnet::Connection& connection, fluxnet::MessageView message) {
            broker.on_message(connection, message);
        });
    running.server.set_on_disconnect(
        [&](fluxnet::Connection& connection) { broker.on_disconnect(connection); });
    running.start();

    ClientThread a(client_cfg(running.server.bound_port()));
    ClientThread b(client_cfg(running.server.bound_port()));
    fluxnet::test::Gate a_gate;
    fluxnet::test::Gate b_gate;
    std::string a_last;
    std::string b_last;
    a.client.set_on_message([&](fluxnet::Connection&, fluxnet::MessageView msg) {
        a_last = text_of(msg);
        a_gate.notify();
    });
    b.client.set_on_message([&](fluxnet::Connection&, fluxnet::MessageView msg) {
        b_last = text_of(msg);
        b_gate.notify();
    });
    a.start();
    b.start();
    auto id_a = a.client.connect();
    auto id_b = b.client.connect();
    ASSERT_TRUE(id_a.valid());
    ASSERT_TRUE(id_b.valid());
    ASSERT_EQ(a.client.send(id_a, fluxnet::MessageType::Data, bytes("SUB topic"), 0),
              fluxnet::SendResult::Accepted);
    ASSERT_EQ(b.client.send(id_b, fluxnet::MessageType::Data, bytes("SUB topic"), 0),
              fluxnet::SendResult::Accepted);
    ASSERT_TRUE(a_gate.wait_for(1, std::chrono::seconds(2)));
    ASSERT_TRUE(b_gate.wait_for(1, std::chrono::seconds(2)));
    EXPECT_EQ(a_last, "OK");
    EXPECT_EQ(b_last, "OK");
    ASSERT_EQ(a.client.send(id_a, fluxnet::MessageType::Data, bytes("PUB topic\nhello"), 0),
              fluxnet::SendResult::Accepted);
    ASSERT_TRUE(b_gate.wait_for(2, std::chrono::seconds(2)));
    EXPECT_EQ(b_last, "MSG topic\nhello");
    // Publisher does not receive its own publish.
    EXPECT_EQ(a_gate.count(), 1);
    ASSERT_EQ(b.client.send(id_b, fluxnet::MessageType::Data, bytes("UNSUB topic"), 0),
              fluxnet::SendResult::Accepted);
    ASSERT_TRUE(b_gate.wait_for(3, std::chrono::seconds(2)));
    const int before = b_gate.count();
    ASSERT_EQ(a.client.send(id_a, fluxnet::MessageType::Data, bytes("PUB topic\nagain"), 0),
              fluxnet::SendResult::Accepted);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(b_gate.count(), before);
    // Stop the server before the broker is destroyed. Otherwise a worker can
    // still be inside on_disconnect while the topic map is freed.
    running.server.stop();
    if (running.thread.joinable()) {
        running.thread.join();
    }
    a.client.stop();
    if (a.thread.joinable()) {
        a.thread.join();
    }
    b.client.stop();
    if (b.thread.joinable()) {
        b.thread.join();
    }
}

TEST(PubSub, DisconnectRemovesSubscription) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 2;
    config.io_backend = fluxnet::IoBackend::Epoll;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    ServerThread running(config);
    fluxnet::examples::Broker broker(running.server);
    running.server.set_on_message(
        [&](fluxnet::Connection& connection, fluxnet::MessageView message) {
            broker.on_message(connection, message);
        });
    running.server.set_on_disconnect(
        [&](fluxnet::Connection& connection) { broker.on_disconnect(connection); });
    running.start();

    ClientThread client(client_cfg(running.server.bound_port()));
    fluxnet::test::Gate gate;
    client.client.set_on_message(
        [&](fluxnet::Connection&, fluxnet::MessageView) { gate.notify(); });
    client.start();
    const auto id = client.client.connect();
    ASSERT_TRUE(id.valid());
    ASSERT_EQ(client.client.send(id, fluxnet::MessageType::Data, bytes("SUB only"), 0),
              fluxnet::SendResult::Accepted);
    ASSERT_TRUE(gate.wait_for(1, std::chrono::seconds(2)));
    EXPECT_EQ(broker.topics(), 1u);
    client.client.close(id);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline &&
           running.server.metrics().disconnects < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(running.server.metrics().disconnects, 1u);
    EXPECT_EQ(broker.topics(), 0u);
    running.server.stop();
    if (running.thread.joinable()) {
        running.thread.join();
    }
}

TEST(Kv, SetGetDelete) {
    fluxnet::examples::Store store(128);
    EXPECT_EQ(store.command("PING"), "PONG");
    EXPECT_EQ(store.command("SET a hello"), "OK");
    EXPECT_EQ(store.command("GET a"), "VALUE hello");
    EXPECT_EQ(store.command("DEL a"), "OK");
    EXPECT_EQ(store.command("GET a"), "NOT_FOUND");
    EXPECT_EQ(store.command("SET big " + std::string(200, 'x')), "ERR full");
    EXPECT_EQ(store.bytes(), 0u);
}
