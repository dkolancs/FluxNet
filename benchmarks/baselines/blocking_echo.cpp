#include "net/socket.hpp"
#include "protocol/codec.hpp"

#include <iostream>
#include <vector>

// One connection at a time, blocking reads and writes. This is the slow baseline.
int main(int argc, char** argv) {
    std::uint16_t port = 9100;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--port" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        }
    }
    auto server = fluxnet::detail::Socket::tcp_v4();
    server.set_reuseaddr(true);
    server.bind_v4("127.0.0.1", port);
    server.listen(128);
    std::cerr << "blocking echo on 127.0.0.1:" << server.local_port() << "\n";
    while (true) {
        auto accepted = server.accept_nb();
        if (!accepted.fd) {
            if (accepted.sys_errno == EINTR) {
                continue;
            }
            std::cerr << "accept failed\n";
            return 1;
        }
        fluxnet::detail::Socket client(std::move(accepted.fd));
        client.set_nonblocking(false);
        fluxnet::detail::FrameDecoder decoder(1 << 20, 1 << 20);
        std::vector<std::byte> buf(16 * 1024);
        bool open = true;
        while (open) {
            auto recvd = client.recv_nb(buf);
            if (recvd.status == fluxnet::detail::IoStatus::Eof ||
                recvd.status == fluxnet::detail::IoStatus::Reset ||
                recvd.status == fluxnet::detail::IoStatus::Error) {
                break;
            }
            if (recvd.status != fluxnet::detail::IoStatus::Ok) {
                continue;
            }
            const bool ok = decoder.feed(buf.data(), recvd.n, [&](fluxnet::MessageView msg) {
                std::vector<std::byte> out(8 + msg.payload().size());
                std::size_t written = 0;
                if (!fluxnet::detail::write_frame(out.data(), out.size(), msg.type(), msg.flags(),
                                                  msg.payload(), 1 << 20, written)) {
                    open = false;
                    return false;
                }
                std::size_t sent = 0;
                while (sent < written && open) {
                    auto result = client.send_nb(
                        std::span<const std::byte>(out.data() + sent, written - sent));
                    if (result.status == fluxnet::detail::IoStatus::Ok) {
                        sent += result.n;
                    } else if (result.status == fluxnet::detail::IoStatus::Again) {
                        continue;
                    } else {
                        open = false;
                    }
                }
                return open;
            });
            if (!ok) {
                break;
            }
        }
    }
}
