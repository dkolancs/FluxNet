// Open N idle TCP connections to a FluxNet echo server and hold them.
// Prints how many connects succeeded. The caller samples the server RSS.
// This is not a throughput benchmark.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int connect_one(const char* host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main(int argc, char** argv) {
    const char* host = "127.0.0.1";
    std::uint16_t port = 9000;
    int connections = 1000;
    int hold_s = 2;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--connections" && i + 1 < argc) {
            connections = std::atoi(argv[++i]);
        } else if (arg == "--hold" && i + 1 < argc) {
            hold_s = std::atoi(argv[++i]);
        } else {
            std::cerr << "usage: fluxnet_hold --port P --connections N --hold SECONDS\n";
            return 2;
        }
    }
    std::vector<int> fds;
    fds.reserve(static_cast<std::size_t>(connections));
    int fail = 0;
    int first_errno = 0;
    for (int i = 0; i < connections; ++i) {
        const int fd = connect_one(host, port);
        if (fd < 0) {
            if (first_errno == 0) {
                first_errno = errno;
            }
            ++fail;
            continue;
        }
        fds.push_back(fd);
    }
    std::cout << "connected=" << fds.size() << " failed=" << fail;
    if (first_errno != 0) {
        std::cout << " first_errno=" << first_errno << " (" << std::strerror(first_errno) << ")";
    }
    std::cout << "\n" << std::flush;
    if (hold_s > 0 && !fds.empty()) {
        ::sleep(static_cast<unsigned>(hold_s));
    }
    for (int fd : fds) {
        ::close(fd);
    }
    return fds.empty() ? 1 : 0;
}
