#include "fluxnet/config.hpp"
#include "fluxnet/message.hpp"

#include <stdexcept>

namespace fluxnet {

const char* to_string(IoBackend backend) noexcept {
    switch (backend) {
    case IoBackend::Auto:
        return "auto";
    case IoBackend::Epoll:
        return "epoll";
    case IoBackend::IoUring:
        return "io_uring";
    }
    return "unknown";
}

const char* to_string(ServerState state) noexcept {
    switch (state) {
    case ServerState::Starting:
        return "starting";
    case ServerState::Running:
        return "running";
    case ServerState::Draining:
        return "draining";
    case ServerState::Stopping:
        return "stopping";
    case ServerState::Stopped:
        return "stopped";
    }
    return "unknown";
}

const char* to_string(EpollTrigger trigger) noexcept {
    return trigger == EpollTrigger::Edge ? "edge" : "level";
}

const char* to_string(SendResult result) noexcept {
    switch (result) {
    case SendResult::Accepted:
        return "accepted";
    case SendResult::Backpressured:
        return "backpressured";
    case SendResult::Closed:
        return "closed";
    case SendResult::InvalidMessage:
        return "invalid";
    }
    return "unknown";
}

bool valid_message_type(std::uint16_t raw) noexcept {
    return raw >= static_cast<std::uint16_t>(MessageType::Data) &&
           raw <= static_cast<std::uint16_t>(MessageType::Close);
}

namespace {

void validate_net(const NetConfig& config) {
    if (config.worker_threads < 1 || config.worker_threads > 256) {
        throw std::invalid_argument("worker_threads must be in [1, 256]");
    }
    if (config.queue_capacity < 1) {
        throw std::invalid_argument("queue_capacity must be >= 1");
    }
    if (config.max_connections < 1) {
        throw std::invalid_argument("max_connections must be >= 1");
    }
    if (config.max_frame_size < 1 || config.max_frame_size > (64u << 20)) {
        throw std::invalid_argument("max_frame_size must be in [1, 64 MiB]");
    }
    if (config.output_low_watermark > config.output_high_watermark) {
        throw std::invalid_argument("output_low_watermark must be <= output_high_watermark");
    }
    if (config.output_high_watermark > config.max_pending_output) {
        throw std::invalid_argument("output_high_watermark must be <= max_pending_output");
    }
    if (config.max_input_buffer < 8) {
        throw std::invalid_argument("max_input_buffer must be >= 8");
    }
    if (config.shutdown_timeout.count() < 0 || config.idle_timeout.count() < 0 ||
        config.slow_consumer_timeout.count() < 0) {
        throw std::invalid_argument("timeouts must be >= 0");
    }
}

} // namespace

void validate(const NetConfig& config) {
    validate_net(config);
}

void validate(const ServerConfig& config) {
    validate_net(config);
    if (config.host.empty()) {
        throw std::invalid_argument("host is empty");
    }
    if (config.listen_backlog < 1) {
        throw std::invalid_argument("listen_backlog must be >= 1");
    }
}

void validate(const ClientConfig& config) {
    validate_net(config);
    if (config.host.empty()) {
        throw std::invalid_argument("host is empty");
    }
    if (config.connect_timeout.count() < 0) {
        throw std::invalid_argument("connect_timeout must be >= 0");
    }
}

} // namespace fluxnet
