#pragma once

#include <cstdio>
#include <cstdlib>
#include <utility>

namespace vkr {

enum class LogLevel { Debug, Info, Warn, Error };

inline const char* levelTag(LogLevel l) {
    switch (l) {
        case LogLevel::Debug: return "DBG";
        case LogLevel::Info:  return "INF";
        case LogLevel::Warn:  return "WRN";
        case LogLevel::Error: return "ERR";
    }
    return "???";
}

template <typename... Args>
void logMsg(LogLevel level, const char* fmt, Args&&... args) {
    std::fprintf(stderr, "[%s] ", levelTag(level));
    if constexpr (sizeof...(Args) == 0)
        std::fprintf(stderr, "%s", fmt);
    else
        std::fprintf(stderr, fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

#define VKR_LOG(fmt, ...)  ::vkr::logMsg(::vkr::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define VKR_WARN(fmt, ...) ::vkr::logMsg(::vkr::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define VKR_ERR(fmt, ...)  ::vkr::logMsg(::vkr::LogLevel::Error, fmt, ##__VA_ARGS__)

[[noreturn]] inline void fatal(const char* msg) {
    VKR_ERR("FATAL: %s", msg);
    std::exit(1);
}

} // namespace vkr
