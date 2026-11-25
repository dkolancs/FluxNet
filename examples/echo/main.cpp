#include "fluxnet/server.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 9000;
    config.worker_threads = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--host" && i + 1 < argc) {
            config.host = argv[++i];
        } else if (arg == "--workers" && i + 1 < argc) {
            config.worker_threads = std::atoi(argv[++i]);
        } else if (arg == "--edge") {
            config.epoll_trigger = fluxnet::EpollTrigger::Edge;
        } else if (arg == "--uring") {
            config.io_backend = fluxnet::IoBackend::IoUring;
        } else if (arg == "--epoll") {
            config.io_backend = fluxnet::IoBackend::Epoll;
        } else if (arg == "--affinity") {
            config.cpu_affinity = true;
        } else if (arg == "--no-pool") {
            config.use_buffer_pool = false;
        } else if (arg == "--no-gather") {
            config.scatter_gather = false;
        } else if (arg == "--max-connections" && i + 1 < argc) {
            config.max_connections = static_cast<std::size_t>(std::atoi(argv[++i]));
        } else if (arg == "--least-loaded") {
            config.load_balance = fluxnet::LoadBalance::LeastLoaded;
        } else {
            std::cerr << "usage: fluxnet_echo [--host H] [--port P] [--workers N] [--edge] "
                         "[--uring|--epoll] [--affinity] [--no-pool] [--no-gather] "
                         "[--least-loaded] [--max-connections N]\n";
            return 2;
        }
    }
    fluxnet::Server server(config);
    server.set_on_message([](fluxnet::Connection& connection, fluxnet::MessageView message) {
        if (message.type() == fluxnet::MessageType::Close) {
            connection.close();
            return;
        }
        connection.send(message.type(), message.payload(), message.flags());
    });
    std::cerr << "echo listening backend will be reported after bind\n";
    server.run();
    std::cerr << "echo stopped backend=" << fluxnet::to_string(server.active_backend())
              << " note=" << server.backend_note() << "\n";
    return 0;
}
