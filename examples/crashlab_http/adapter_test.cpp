#include "fluxnet/server.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

std::string adapter_path() {
    const char* env = std::getenv("FLUXNET_ADAPTER");
    return env == nullptr ? "fluxnet_crashlab_http" : env;
}

} // namespace

TEST(CrashLabHttp, PingBecomesHttp200) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 1;
    config.idle_timeout = std::chrono::milliseconds(0);
    config.slow_consumer_timeout = std::chrono::milliseconds(0);
    fluxnet::Server server(config);
    server.set_on_message([](fluxnet::Connection& conn, fluxnet::MessageView msg) {
        conn.send(msg.type(), msg.payload(), msg.flags());
    });
    std::thread thread([&] { server.run(); });
    struct Stop {
        fluxnet::Server* server = nullptr;
        std::thread* thread = nullptr;
        ~Stop() {
            std::system("if [ -f /tmp/fluxnet-adapter.pid ]; then kill $(cat "
                        "/tmp/fluxnet-adapter.pid) >/dev/null 2>&1 || true; fi");
            if (server != nullptr) {
                server->stop();
            }
            if (thread != nullptr && thread->joinable()) {
                thread->join();
            }
        }
    } stop{&server, &thread};
    ASSERT_TRUE(server.wait_until_running(std::chrono::seconds(2)));
    const int listen_port = 18081;
    const std::string cmd = adapter_path() + " --listen " + std::to_string(listen_port) +
                            " --echo-host 127.0.0.1 --echo-port " +
                            std::to_string(server.bound_port()) + " >/tmp/fluxnet-adapter.log 2>&1";
    const int child = std::system((cmd + " & echo $! > /tmp/fluxnet-adapter.pid").c_str());
    ASSERT_EQ(child, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    int fd = -1;
    for (int attempt = 0; attempt < 20; ++attempt) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            break;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<std::uint16_t>(listen_port));
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_GE(fd, 0);
    const char* request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    ASSERT_GT(::send(fd, request, std::strlen(request), MSG_NOSIGNAL), 0);
    char buf[256]{};
    const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    ::close(fd);
    ASSERT_GT(n, 0);
    EXPECT_NE(std::string(buf).find("200"), std::string::npos) << buf;
}
