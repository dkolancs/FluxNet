#include "net/socket.hpp"

#include <gtest/gtest.h>

#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <thread>
#include <vector>

using fluxnet::detail::IoStatus;
using fluxnet::detail::Socket;

TEST(Socket, LoopbackSendRecv) {
    Socket server = Socket::tcp_v4();
    server.set_reuseaddr(true);
    server.bind_v4("127.0.0.1", 0);
    server.listen(16);
    const auto port = server.local_port();

    std::thread client_thread([&] {
        Socket client = Socket::tcp_v4();
        auto result = client.connect_v4("127.0.0.1", port);
        ASSERT_EQ(result.status, fluxnet::detail::ConnectStatus::Connected);
        const auto payload = std::vector<std::byte>{std::byte{1}, std::byte{2}, std::byte{3}};
        auto sent = client.send_nb(payload);
        EXPECT_EQ(sent.status, IoStatus::Ok);
        EXPECT_EQ(sent.n, payload.size());
    });

    auto accepted = server.accept_nb();
    // The listen socket is blocking by default, but accept4 may still race. Retry briefly.
    for (int i = 0; i < 50 && !accepted.fd && accepted.sys_errno == EAGAIN; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        accepted = server.accept_nb();
    }
    ASSERT_TRUE(accepted.fd) << accepted.sys_errno;
    Socket peer(std::move(accepted.fd));
    std::byte buf[8] = {};
    IoStatus status = IoStatus::Again;
    std::size_t got = 0;
    for (int i = 0; i < 50 && got < 3; ++i) {
        auto recvd = peer.recv_nb(buf);
        if (recvd.status == IoStatus::Ok) {
            got += recvd.n;
            status = recvd.status;
        } else if (recvd.status == IoStatus::Again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } else {
            status = recvd.status;
            break;
        }
    }
    EXPECT_EQ(status, IoStatus::Ok);
    EXPECT_EQ(got, 3u);
    client_thread.join();
}

TEST(Socket, NonblockingRecvAgain) {
    Socket server = Socket::tcp_v4();
    server.set_reuseaddr(true);
    server.bind_v4("127.0.0.1", 0);
    server.listen(1);
    Socket client = Socket::tcp_v4();
    ASSERT_EQ(client.connect_v4("127.0.0.1", server.local_port()).status,
              fluxnet::detail::ConnectStatus::Connected);
    auto accepted = server.accept_nb();
    ASSERT_TRUE(accepted.fd);
    Socket peer(std::move(accepted.fd));
    peer.set_nonblocking(true);
    std::byte buf[4];
    auto recvd = peer.recv_nb(buf);
    EXPECT_EQ(recvd.status, IoStatus::Again);
}

TEST(Socket, ConnectRefused) {
    Socket client = Socket::tcp_v4();
    auto result = client.connect_v4("127.0.0.1", 1);
    EXPECT_EQ(result.status, fluxnet::detail::ConnectStatus::Failed);
    EXPECT_TRUE(result.sys_errno == ECONNREFUSED || result.sys_errno == EAGAIN);
}

TEST(Socket, NodelayAndReuse) {
    Socket socket = Socket::tcp_v4();
    EXPECT_NO_THROW(socket.set_nodelay(true));
    EXPECT_NO_THROW(socket.set_reuseaddr(true));
    int value = 0;
    socklen_t len = sizeof(value);
    ASSERT_EQ(::getsockopt(socket.fd(), IPPROTO_TCP, TCP_NODELAY, &value, &len), 0);
    EXPECT_EQ(value, 1);
}
