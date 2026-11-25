// Boost.Asio echo baseline. Not part of libfluxnet.
// Same 8-byte big-endian frame as FluxNet: u32 payload length, u16 type, u16 flags.
// TCP_NODELAY. The bytes of each request are written back unchanged.
// One io_context thread accepts and serves. This is not linked into libfluxnet.

#include <boost/asio.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace {

constexpr std::size_t kMaxPayload = 1u << 20;

class Session : public std::enable_shared_from_this<Session> {
  public:
    explicit Session(tcp::socket socket) : socket_(std::move(socket)) {
        socket_.set_option(tcp::no_delay(true));
    }

    void start() { read_header(); }

  private:
    void read_header() {
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(header_),
                         [this, self](boost::system::error_code ec, std::size_t) {
                             if (ec) {
                                 return;
                             }
                             const std::uint32_t len =
                                 (static_cast<std::uint32_t>(header_[0]) << 24) |
                                 (static_cast<std::uint32_t>(header_[1]) << 16) |
                                 (static_cast<std::uint32_t>(header_[2]) << 8) |
                                 static_cast<std::uint32_t>(header_[3]);
                             if (len > kMaxPayload) {
                                 boost::system::error_code ignore;
                                 socket_.close(ignore);
                                 return;
                             }
                             payload_.resize(len);
                             if (len == 0) {
                                 write_frame();
                                 return;
                             }
                             read_payload();
                         });
    }

    void read_payload() {
        auto self = shared_from_this();
        asio::async_read(socket_, asio::buffer(payload_),
                         [this, self](boost::system::error_code ec, std::size_t) {
                             if (ec) {
                                 return;
                             }
                             write_frame();
                         });
    }

    void write_frame() {
        frame_.resize(8 + payload_.size());
        for (int i = 0; i < 8; ++i) {
            frame_[static_cast<std::size_t>(i)] = header_[static_cast<std::size_t>(i)];
        }
        if (!payload_.empty()) {
            std::memcpy(frame_.data() + 8, payload_.data(), payload_.size());
        }
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(frame_),
                          [this, self](boost::system::error_code ec, std::size_t) {
                              if (ec) {
                                  return;
                              }
                              read_header();
                          });
    }

    tcp::socket socket_;
    std::array<unsigned char, 8> header_{};
    std::vector<unsigned char> payload_;
    std::vector<unsigned char> frame_;
};

class Server {
  public:
    Server(asio::io_context& io, unsigned short port)
        : acceptor_(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), port)) {
        acceptor_.set_option(tcp::acceptor::reuse_address(true));
        std::cerr << "asio echo on 127.0.0.1:" << acceptor_.local_endpoint().port() << "\n";
        accept();
    }

  private:
    void accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                std::make_shared<Session>(std::move(socket))->start();
            }
            if (acceptor_.is_open()) {
                accept();
            }
        });
    }

    tcp::acceptor acceptor_;
};

} // namespace

int main(int argc, char** argv) {
    unsigned short port = 9100;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = static_cast<unsigned short>(std::atoi(argv[++i]));
        }
    }
    try {
        asio::io_context io(1);
        Server server(io, port);
        io.run();
    } catch (const std::exception& ex) {
        std::cerr << "asio echo: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
