#pragma once

#include "fluxnet/config.hpp"

namespace fluxnet::detail {

class Logger {
  public:
    static void set_level(LogLevel level) noexcept;
    static LogLevel level() noexcept;
    static void log(LogLevel level, const char* fmt, ...) noexcept;
};

} // namespace fluxnet::detail
