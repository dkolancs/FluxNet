#include "pubsub/broker.hpp"

#include <iostream>

int main(int argc, char** argv) {
    fluxnet::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = 9001;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--workers" && i + 1 < argc) {
            config.worker_threads = std::atoi(argv[++i]);
        }
    }
    fluxnet::Server server(config);
    fluxnet::examples::Broker broker(server);
    server.set_on_message([&](fluxnet::Connection& connection, fluxnet::MessageView message) {
        broker.on_message(connection, message);
    });
    server.set_on_disconnect(
        [&](fluxnet::Connection& connection) { broker.on_disconnect(connection); });
    std::cerr << "pubsub on " << config.host << ":" << config.port << "\n";
    server.run();
    return 0;
}
