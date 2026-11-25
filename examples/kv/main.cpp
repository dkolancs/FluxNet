#include "kv/store.hpp"
#include "fluxnet/server.hpp"

#include <iostream>

int main(int argc, char** argv) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 9002;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--workers" && i + 1 < argc) {
            config.worker_threads = std::atoi(argv[++i]);
        }
    }
    fluxnet::examples::Store store;
    fluxnet::Server server(config);
    server.set_on_message([&](fluxnet::Connection& connection, fluxnet::MessageView message) {
        const auto response = store.command(fluxnet::examples::payload_text(message.payload()));
        auto bytes = fluxnet::examples::to_bytes(response);
        connection.send(fluxnet::MessageType::Data, bytes, 0);
    });
    std::cerr << "kv on " << config.host << ":" << config.port << "\n";
    server.run();
    return 0;
}
