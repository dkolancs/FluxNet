#include "net/socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace fluxnet::detail {
namespace {

[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

void set_flag(int fd, int level, int name, int value, const char* what) {
    if (::setsockopt(fd, level, name, &value, sizeof(value)) < 0) {
        throw_errno(what);
    }
}

sockaddr_in parse_v4(const std::string& host, std::uint16_t port, const char* what) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        throw std::invalid_argument(std::string(what) + ": expected an IPv4 address, got " + host);
    }
    return addr;
}

IoResult map_io(ssize_t n) {
    IoResult r;
    if (n > 0) {
        r.status = IoStatus::Ok;
        r.n = static_cast<std::size_t>(n);
        return r;
    }
    if (n == 0) {
        r.status = IoStatus::Eof;
        return r;
    }
    if (errno == EINTR) {
        r.status = IoStatus::Again;
        r.sys_errno = EINTR;
        return r;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        r.status = IoStatus::Again;
        r.sys_errno = errno;
        return r;
    }
    if (errno == ECONNRESET || errno == EPIPE) {
        r.status = IoStatus::Reset;
        r.sys_errno = errno;
        return r;
    }
    r.status = IoStatus::Error;
    r.sys_errno = errno;
    return r;
}

} // namespace

std::string format_endpoint(const ::sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

Socket Socket::tcp_v4() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw_errno("socket");
    }
    return Socket(FileDescriptor(fd));
}

void Socket::set_nonblocking(bool enabled) {
    int flags = ::fcntl(fd(), F_GETFL, 0);
    if (flags < 0) {
        throw_errno("fcntl(F_GETFL)");
    }
    if (enabled) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    if (::fcntl(fd(), F_SETFL, flags) < 0) {
        throw_errno("fcntl(F_SETFL)");
    }
}

void Socket::set_nodelay(bool enabled) {
    set_flag(fd(), IPPROTO_TCP, TCP_NODELAY, enabled ? 1 : 0, "TCP_NODELAY");
}

void Socket::set_reuseaddr(bool enabled) {
    set_flag(fd(), SOL_SOCKET, SO_REUSEADDR, enabled ? 1 : 0, "SO_REUSEADDR");
}

void Socket::set_reuseport(bool enabled) {
    set_flag(fd(), SOL_SOCKET, SO_REUSEPORT, enabled ? 1 : 0, "SO_REUSEPORT");
}

void Socket::set_cloexec(bool enabled) {
    int flags = ::fcntl(fd(), F_GETFD, 0);
    if (flags < 0) {
        throw_errno("fcntl(F_GETFD)");
    }
    if (enabled) {
        flags |= FD_CLOEXEC;
    } else {
        flags &= ~FD_CLOEXEC;
    }
    if (::fcntl(fd(), F_SETFD, flags) < 0) {
        throw_errno("fcntl(F_SETFD)");
    }
}

void Socket::bind_v4(const std::string& host, std::uint16_t port) {
    auto addr = parse_v4(host, port, "bind");
    if (::bind(fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        throw_errno("bind");
    }
}

void Socket::listen(int backlog) {
    if (::listen(fd(), backlog) < 0) {
        throw_errno("listen");
    }
}

ConnectResult Socket::connect_v4(const std::string& host, std::uint16_t port) {
    auto addr = parse_v4(host, port, "connect");
    ConnectResult result;
    for (;;) {
        if (::connect(fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            result.status = ConnectStatus::Connected;
            return result;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EINPROGRESS) {
            result.status = ConnectStatus::InProgress;
            return result;
        }
        result.status = ConnectStatus::Failed;
        result.sys_errno = errno;
        return result;
    }
}

Socket::AcceptResult Socket::accept_nb() {
    AcceptResult result;
    for (;;) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int fd = ::accept4(fd_.get(), reinterpret_cast<sockaddr*>(&peer), &len,
                           SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) {
            result.fd = FileDescriptor(fd);
            return result;
        }
        if (errno == EINTR) {
            continue;
        }
        result.sys_errno = errno;
        return result;
    }
}

IoResult Socket::send_nb(std::span<const std::byte> data) {
    for (;;) {
        ssize_t n = ::send(fd(), data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return map_io(n);
    }
}

IoResult Socket::recv_nb(std::span<std::byte> data) {
    for (;;) {
        ssize_t n = ::recv(fd(), data.data(), data.size(), 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return map_io(n);
    }
}

IoResult Socket::sendmsg_nb(const iovec* iov, int count) {
    msghdr msg{};
    msg.msg_iov = const_cast<iovec*>(iov);
    msg.msg_iovlen = static_cast<std::size_t>(count);
    for (;;) {
        ssize_t n = ::sendmsg(fd(), &msg, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return map_io(n);
    }
}

void Socket::shutdown_write() {
    if (::shutdown(fd(), SHUT_WR) < 0 && errno != ENOTCONN) {
        throw_errno("shutdown");
    }
}

int Socket::socket_error() const {
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd(), SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
        return errno;
    }
    return err;
}

std::uint16_t Socket::local_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd(), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        throw_errno("getsockname");
    }
    return ntohs(addr.sin_port);
}

std::string Socket::peer_string() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(fd(), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return {};
    }
    return format_endpoint(addr);
}

std::string Socket::local_string() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd(), reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
        return {};
    }
    return format_endpoint(addr);
}

} // namespace fluxnet::detail
