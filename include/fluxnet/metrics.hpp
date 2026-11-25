#pragma once

#include <cstdint>

namespace fluxnet {

// Relaxed counters. Safe to read while the server runs; values can tear across fields.
struct MetricsSnapshot {
    std::uint64_t accepts = 0;
    std::uint64_t connects = 0;
    std::uint64_t disconnects = 0;
    std::uint64_t messages_in = 0;
    std::uint64_t messages_out = 0;
    std::uint64_t bytes_in = 0;
    std::uint64_t bytes_out = 0;
    std::uint64_t send_backpressured = 0;
    std::uint64_t send_invalid = 0;
    std::uint64_t send_closed = 0;
    std::uint64_t dropped_handoff = 0;
    std::uint64_t rejected_admission = 0;
    std::uint64_t protocol_errors = 0;
    std::uint64_t callback_exceptions = 0;
    std::uint64_t timeouts_idle = 0;
    std::uint64_t timeouts_slow = 0;
    std::uint64_t active_connections = 0;
    std::uint64_t pool_acquires = 0;
    std::uint64_t pool_reuses = 0;
    std::uint64_t pool_allocs = 0;
    std::uint64_t pool_bytes_in_use = 0;
    std::uint64_t pool_bytes_high_watermark = 0;
    std::uint64_t task_queue_full = 0;
    std::uint64_t sq_full = 0;
};

} // namespace fluxnet
