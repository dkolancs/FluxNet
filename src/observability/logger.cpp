#include "observability/logger.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace fluxnet::detail {
namespace {
std::mutex g_mu;
std::atomic<int> g_level{static_cast<int>(LogLevel::Warn)};

const char* name_of(LogLevel level) {
    switch (level) {
    case LogLevel::Error:
        return "error";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Info:
        return "info";
    case LogLevel::Debug:
        return "debug";
    }
    return "log";
}
} // namespace

void Logger::set_level(LogLevel level) noexcept {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel Logger::level() noexcept {
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

void Logger::log(LogLevel level, const char* fmt, ...) noexcept {
    if (static_cast<int>(level) > g_level.load(std::memory_order_relaxed)) {
        return;
    }
    char body[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_mu);
    std::fprintf(stderr, "{\"level\":\"%s\",\"msg\":\"%s\"}\n", name_of(level), body);
}

} // namespace fluxnet::detail
