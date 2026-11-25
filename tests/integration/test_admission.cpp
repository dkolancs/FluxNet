#include "fluxnet/client.hpp"
#include "fluxnet/server.hpp"
#include "net/socket.hpp"
#include "support/wait.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/time.h>

namespace {

fluxnet::ServerConfig capped() {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.io_backend = fluxnet::IoBackend::Epoll;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    config.max_pending_output = 64;
    config.output_high_watermark = 64;
    config.output_low_watermark = 16;
    config.queue_capacity = 1024;
    config.max_frame_size = 1024;
    config.shutdown_timeout = std::chrono::milliseconds(50);
    return config;
}

} // namespace

// The old try_send returned Accepted as soon as a task was queued. The worker
// later dropped the frame when the output cap was hit, so Accepted exceeded
// max_pending_output and messages_out lagged behind. Admission now happens
// before Accepted, even while the owner is blocked inside on_connect.
TEST(Admission, TrySendDoesNotAcceptPastCap) {
    auto config = capped();
    fluxnet::Server server(config);
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    fluxnet::ConnectionId id;
    server.set_on_connect([&](fluxnet::Connection& connection) {
        {
            std::lock_guard lock(mu);
            id = connection.id();
            entered = true;
        }
        cv.notify_all();
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return release; });
    });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));

    fluxnet::detail::Socket client = fluxnet::detail::Socket::tcp_v4();
    ASSERT_EQ(client.connect_v4("127.0.0.1", server.bound_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    {
        std::unique_lock lock(mu);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    ASSERT_TRUE(id.valid());

    const std::vector<std::byte> payload(8, std::byte{0x11});
    constexpr std::size_t kFrame = 16;
    int accepted = 0;
    int backpressured = 0;
    for (int i = 0; i < 16; ++i) {
        const auto result = server.try_send(id, fluxnet::MessageType::Data, payload, 0);
        if (result == fluxnet::SendResult::Accepted) {
            ++accepted;
        } else if (result == fluxnet::SendResult::Backpressured) {
            ++backpressured;
        } else {
            ADD_FAILURE() << "unexpected " << fluxnet::to_string(result);
        }
    }
    EXPECT_EQ(accepted, 4);
    EXPECT_EQ(backpressured, 12);
    EXPECT_LE(static_cast<std::size_t>(accepted) * kFrame, config.max_pending_output);
    EXPECT_EQ(server.metrics().messages_out, static_cast<std::uint64_t>(accepted));
    const auto back_before = server.metrics().send_backpressured;
    EXPECT_EQ(back_before, static_cast<std::uint64_t>(backpressured));

    {
        std::lock_guard lock(mu);
        release = true;
    }
    cv.notify_all();

    timeval tv{};
    tv.tv_sec = 2;
    ::setsockopt(client.fd(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::vector<std::byte> got;
    const std::size_t expect_bytes = static_cast<std::size_t>(accepted) * kFrame;
    while (got.size() < expect_bytes) {
        std::byte tmp[64];
        const auto recvd = client.recv_nb(tmp);
        if (recvd.status != fluxnet::detail::IoStatus::Ok || recvd.n == 0) {
            break;
        }
        got.insert(got.end(), tmp, tmp + recvd.n);
    }
    EXPECT_EQ(got.size(), expect_bytes);
    server.stop();
    thread.join();
}

TEST(Dispatch, LeastLoadedReturnsToIdleWorker) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 2;
    config.load_balance = fluxnet::LoadBalance::LeastLoaded;
    config.io_backend = fluxnet::IoBackend::Epoll;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    config.shutdown_timeout = std::chrono::milliseconds(50);
    fluxnet::Server server(config);
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::uint32_t> workers;
    server.set_on_connect([&](fluxnet::Connection& connection) {
        std::lock_guard lock(mu);
        workers.push_back(connection.id().worker);
        cv.notify_all();
    });
    fluxnet::test::Gate closed;
    server.set_on_disconnect([&](fluxnet::Connection&) { closed.notify(); });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));

    auto wait_n = [&](std::size_t n) {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, std::chrono::seconds(2), [&] { return workers.size() >= n; });
    };

    fluxnet::detail::Socket first = fluxnet::detail::Socket::tcp_v4();
    ASSERT_EQ(first.connect_v4("127.0.0.1", server.bound_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    ASSERT_TRUE(wait_n(1));
    first = fluxnet::detail::Socket();
    ASSERT_TRUE(closed.wait_for(1, std::chrono::seconds(2)));
    for (int i = 0; i < 1000 && server.metrics().active_connections != 0; ++i) {
        std::this_thread::yield();
    }
    ASSERT_EQ(server.metrics().active_connections, 0u);

    fluxnet::detail::Socket second = fluxnet::detail::Socket::tcp_v4();
    ASSERT_EQ(second.connect_v4("127.0.0.1", server.bound_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    ASSERT_TRUE(wait_n(2));
    std::uint32_t a = 0;
    std::uint32_t b = 1;
    {
        std::lock_guard lock(mu);
        ASSERT_EQ(workers.size(), 2u);
        a = workers[0];
        b = workers[1];
    }
    // Both loads are 0, so the scan keeps the lowest index. Round-robin would
    // have advanced and placed the second connection on the other worker.
    EXPECT_EQ(a, b);
    server.stop();
    thread.join();
}

TEST(Admission, ConcurrentTrySendMatchesCap) {
    auto config = capped();
    fluxnet::Server server(config);
    std::mutex mu;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    fluxnet::ConnectionId id;
    server.set_on_connect([&](fluxnet::Connection& connection) {
        {
            std::lock_guard lock(mu);
            id = connection.id();
            entered = true;
        }
        cv.notify_all();
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return release; });
    });
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    fluxnet::detail::Socket client = fluxnet::detail::Socket::tcp_v4();
    ASSERT_EQ(client.connect_v4("127.0.0.1", server.bound_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    {
        std::unique_lock lock(mu);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    std::atomic<int> accepted{0};
    std::atomic<int> backpressured{0};
    const std::vector<std::byte> payload(8, std::byte{1});
    std::vector<std::thread> senders;
    for (int t = 0; t < 4; ++t) {
        senders.emplace_back([&] {
            for (int i = 0; i < 8; ++i) {
                const auto result = server.try_send(id, fluxnet::MessageType::Data, payload, 0);
                if (result == fluxnet::SendResult::Accepted) {
                    accepted.fetch_add(1);
                } else if (result == fluxnet::SendResult::Backpressured) {
                    backpressured.fetch_add(1);
                }
            }
        });
    }
    for (auto& sender : senders) {
        sender.join();
    }
    EXPECT_EQ(accepted.load() + backpressured.load(), 32);
    EXPECT_LE(accepted.load(), 4);
    EXPECT_GE(backpressured.load(), 28);
    {
        std::lock_guard lock(mu);
        release = true;
    }
    cv.notify_all();
    server.stop();
    thread.join();
}

TEST(Pool, EchoUsesPoolAndReleases) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    config.use_buffer_pool = true;
    fluxnet::Server server(config);
    fluxnet::test::Gate gate;
    server.set_on_message([&](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        conn.send(msg.type(), msg.payload(), msg.flags());
        gate.notify();
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
    std::vector<std::byte> payload{std::byte{7}};
    ASSERT_EQ(client.send(id, fluxnet::MessageType::Data, payload, 0),
              fluxnet::SendResult::Accepted);
    ASSERT_TRUE(gate.wait_for(1, std::chrono::seconds(2)));
    EXPECT_GT(server.metrics().pool_acquires, 0u);
    client.close(id);
    client.stop();
    client_thread.join();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && server.metrics().pool_bytes_in_use != 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(server.metrics().pool_bytes_in_use, 0u);
    if (const char* path = std::getenv("FLUXNET_POOL_NOTE")) {
        std::ofstream out(path);
        out << "pool_acquires=" << server.metrics().pool_acquires
            << " pool_reuses=" << server.metrics().pool_reuses
            << " pool_allocs=" << server.metrics().pool_allocs
            << " pool_bytes_in_use=" << server.metrics().pool_bytes_in_use
            << " pool_bytes_high_watermark=" << server.metrics().pool_bytes_high_watermark << "\n";
    }
    server.stop();
    thread.join();
}

TEST(Pool, DisabledPoolDoesNotAcquire) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    config.use_buffer_pool = false;
    fluxnet::Server server(config);
    fluxnet::test::Gate gate;
    server.set_on_message([&](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        conn.send(msg.type(), msg.payload(), msg.flags());
        gate.notify();
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
    std::vector<std::byte> payload{std::byte{7}};
    ASSERT_EQ(client.send(id, fluxnet::MessageType::Data, payload, 0),
              fluxnet::SendResult::Accepted);
    ASSERT_TRUE(gate.wait_for(1, std::chrono::seconds(2)));
    EXPECT_EQ(server.metrics().pool_reuses, 0u);
    if (const char* path = std::getenv("FLUXNET_POOL_NOTE")) {
        std::ofstream out(path, std::ios::app);
        out << "nopool_acquires=" << server.metrics().pool_acquires
            << " nopool_reuses=" << server.metrics().pool_reuses << "\n";
    }
    client.stop();
    client_thread.join();
    server.stop();
    thread.join();
}

TEST(Shutdown, StopWhileConnecting) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.shutdown_timeout = std::chrono::milliseconds(200);
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::Server server(config);
    std::thread thread([&] { server.run(); });
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    std::atomic<bool> go{true};
    std::thread churn([&] {
        while (go.load()) {
            fluxnet::detail::Socket socket = fluxnet::detail::Socket::tcp_v4();
            socket.connect_v4("127.0.0.1", server.bound_port());
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    server.stop();
    go.store(false);
    churn.join();
    thread.join();
    EXPECT_EQ(server.state(), fluxnet::ServerState::Stopped);
}
