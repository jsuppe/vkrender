#pragma once

#include <cstdio>
#include <cstdlib>
#include <utility>

#ifdef __ANDROID__
#include <android/log.h>
#endif

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
#ifdef __ANDROID__
    int prio = level == LogLevel::Error  ? ANDROID_LOG_ERROR
               : level == LogLevel::Warn ? ANDROID_LOG_WARN
                                         : ANDROID_LOG_INFO;
    if constexpr (sizeof...(Args) == 0)
        __android_log_print(prio, "vkrender", "%s", fmt);
    else
        __android_log_print(prio, "vkrender", fmt, std::forward<Args>(args)...);
#else
    std::fprintf(stderr, "[%s] ", levelTag(level));
    if constexpr (sizeof...(Args) == 0)
        std::fprintf(stderr, "%s", fmt);
    else
        std::fprintf(stderr, fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
#endif
}

#define VKR_LOG(fmt, ...)  ::vkr::logMsg(::vkr::LogLevel::Info,  fmt, ##__VA_ARGS__)
#define VKR_WARN(fmt, ...) ::vkr::logMsg(::vkr::LogLevel::Warn,  fmt, ##__VA_ARGS__)
#define VKR_ERR(fmt, ...)  ::vkr::logMsg(::vkr::LogLevel::Error, fmt, ##__VA_ARGS__)

[[noreturn]] inline void fatal(const char* msg) {
    VKR_ERR("FATAL: %s", msg);
    std::exit(1);
}

} // namespace vkr
