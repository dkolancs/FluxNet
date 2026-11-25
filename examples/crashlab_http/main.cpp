#include "protocol/codec.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

int connect_echo(const std::string& host, std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool echo_ping(const std::string& host, std::uint16_t port) {
    int fd = connect_echo(host, port);
    if (fd < 0) {
        return false;
    }
    std::byte frame[8]{};
    const std::size_t cap = sizeof(frame);
    std::size_t written = 0;
    std::span<const std::byte> empty;
    const bool ok = fluxnet::detail::write_frame(frame, cap, fluxnet::MessageType::Ping, 0, empty,
                                                 1024, written);
    if (!ok || written != 8) {
        ::close(fd);
        return false;
    }
    std::size_t sent = 0;
    while (sent < written) {
        const ssize_t n = ::send(fd, frame + sent, written - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    std::byte reply[8]{};
    std::size_t got = 0;
    while (got < 8) {
        const ssize_t n = ::recv(fd, reply + got, 8 - got, 0);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return fluxnet::detail::read_be16(reply + 4) ==
           static_cast<std::uint16_t>(fluxnet::MessageType::Ping);
}

void http_reply(int fd, int status, const char* text) {
    const std::string body = text;
    const std::string msg = "HTTP/1.1 " + std::to_string(status) +
                            (status == 200 ? " OK" : " Bad Gateway") +
                            "\r\nContent-Length: " + std::to_string(body.size()) +
                            "\r\nConnection: close\r\n\r\n" + body;
    ::send(fd, msg.data(), msg.size(), MSG_NOSIGNAL);
}

} // namespace

// HTTP front for CrashLab's entry probe. CrashLab speaks HTTP. FluxNet echo speaks
// the 8-byte frame protocol. This process translates one HTTP request into one
// framed PING and reports the echo result. It does not change CrashLab.
int main(int argc, char** argv) {
    std::string echo_host = "127.0.0.1";
    std::uint16_t echo_port = 9000;
    std::uint16_t listen_port = 8081;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--echo-host" && i + 1 < argc) {
            echo_host = argv[++i];
        } else if (arg == "--echo-port" && i + 1 < argc) {
            echo_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--listen" && i + 1 < argc) {
            listen_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else {
            std::cerr
                << "usage: fluxnet_crashlab_http [--listen P] [--echo-host H] [--echo-port P]\n";
            return 2;
        }
    }
    int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        return 1;
    }
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd, 16) != 0) {
        std::cerr << "listen failed\n";
        return 1;
    }
    std::cerr << "crashlab http adapter on " << listen_port << " -> " << echo_host << ":"
              << echo_port << "\n";
    for (;;) {
        int client = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            continue;
        }
        char buf[1024];
        ::recv(client, buf, sizeof(buf), 0);
        const bool healthy = echo_ping(echo_host, echo_port);
        http_reply(client, healthy ? 200 : 502, healthy ? "ok" : "echo failed");
        ::close(client);
    }
}
