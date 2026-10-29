#pragma once
#define FL_TRACE_NS vkrtrace
#define FL_TRACE_ENV "VKR_TRACE"
#include <framelink_trace.hpp>

namespace vkrtrace {
inline bool gpuEnabled() noexcept {
    static const bool on=[] {const char* p=std::getenv("VKR_TRACE_GPU");return p && p[0]=='1';}();
    return enabled() && on;
}
}
