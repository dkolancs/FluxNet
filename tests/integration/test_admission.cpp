#include "fluxnet/server.hpp"
#include "net/socket.hpp"
#include "support/wait.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
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
