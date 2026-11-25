#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fluxnet {

enum class IoBackend {
    Auto,
    Epoll,
    IoUring,
};

enum class EpollTrigger {
    Level,
    Edge,
};

enum class LoadBalance {
    RoundRobin,
    LeastLoaded,
};

enum class LogLevel {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
};

enum class ServerState {
    Starting,
    Running,
    Draining,
    Stopping,
    Stopped,
};

// Shared knobs for Server and Client. Zero durations disable that timer.
struct NetConfig {
    int worker_threads = 1;
    IoBackend io_backend = IoBackend::Auto;
    EpollTrigger epoll_trigger = EpollTrigger::Level;
    std::size_t queue_capacity = 1024;
    std::size_t max_connections = 10000;
    std::size_t max_frame_size = 1u << 20; // 1 MiB payload
    std::size_t pool_max_per_class = 256;
    std::size_t output_high_watermark = 256u * 1024u;
    std::size_t output_low_watermark = 64u * 1024u;
    std::size_t max_pending_output = 1u << 20;
    std::size_t max_input_buffer = 1u << 20;
    std::chrono::milliseconds idle_timeout{60000};
    std::chrono::milliseconds slow_consumer_timeout{5000};
    std::chrono::milliseconds shutdown_timeout{2000};
    bool tcp_nodelay = true;
    bool cpu_affinity = false;
    bool use_buffer_pool = true;
    bool scatter_gather = true;
    LogLevel log_level = LogLevel::Warn;
};

struct ServerConfig : NetConfig {
    std::string host = "0.0.0.0";
    std::uint16_t port = 9000;
    bool reuse_addr = true;
    bool reuse_port = false;
    int listen_backlog = 512;
    LoadBalance load_balance = LoadBalance::RoundRobin;
    // Accept handoff. True: lock-free SPSC (one producer). False: mutex queue.
    bool spsc_handoff = true;
};

struct ClientConfig : NetConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 9000;
    std::chrono::milliseconds connect_timeout{2000};
};

const char* to_string(IoBackend backend) noexcept;
const char* to_string(ServerState state) noexcept;
const char* to_string(EpollTrigger trigger) noexcept;

// Throws std::invalid_argument when watermarks, sizes, or thread counts cannot be honored.
void validate(const NetConfig& config);
void validate(const ServerConfig& config);
void validate(const ClientConfig& config);

} // namespace fluxnet
