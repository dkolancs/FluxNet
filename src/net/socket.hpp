#pragma once

#include "net/file_descriptor.hpp"

#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <sys/uio.h>

namespace fluxnet::detail {

enum class IoStatus { Ok, Again, Eof, Reset, Error };

struct IoResult {
    IoStatus status = IoStatus::Ok;
    std::size_t n = 0;
    int sys_errno = 0;
};

enum class ConnectStatus { Connected, InProgress, Failed };

struct ConnectResult {
    ConnectStatus status = ConnectStatus::Failed;
    int sys_errno = 0;
};

class Socket {
  public:
    Socket() = default;
    explicit Socket(FileDescriptor fd) noexcept : fd_(std::move(fd)) {}

    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&&) noexcept = default;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    static Socket tcp_v4();

    int fd() const noexcept { return fd_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(fd_); }
    FileDescriptor release() noexcept { return std::move(fd_); }

    void set_nonblocking(bool enabled);
    void set_nodelay(bool enabled);
    void set_reuseaddr(bool enabled);
    void set_reuseport(bool enabled);
    void set_cloexec(bool enabled);

    void bind_v4(const std::string& host, std::uint16_t port);
    void listen(int backlog);
    ConnectResult connect_v4(const std::string& host, std::uint16_t port);

    // accept4. On failure fd is empty and sys_errno is set (EAGAIN, EMFILE, ...).
    struct AcceptResult {
        FileDescriptor fd;
        int sys_errno = 0;
    };
    AcceptResult accept_nb();

    IoResult send_nb(std::span<const std::byte> data);
    IoResult recv_nb(std::span<std::byte> data);
    IoResult sendmsg_nb(const struct iovec* iov, int count);

    void shutdown_write();
    int socket_error() const;
    std::uint16_t local_port() const;
    std::string peer_string() const;
    std::string local_string() const;

  private:
    FileDescriptor fd_;
};

std::string format_endpoint(const ::sockaddr_in& addr);

} // namespace fluxnet::detail
