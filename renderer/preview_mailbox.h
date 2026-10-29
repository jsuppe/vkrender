#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>
namespace vkr {
// Request/result exchange only: Binder callers never wait for a GPU fence.
class PreviewMailbox {
 public:
    struct Result {uint32_t width=0,height=0,sequence=0;std::vector<uint32_t> argb;};
    void supported(bool value){std::lock_guard<std::mutex> lock(m_);if(supported_!=value){++epoch_;result_={};requested_=false;}supported_=value;}
    bool supported(){std::lock_guard<std::mutex> lock(m_);return supported_;}
    void ready(bool value){std::lock_guard<std::mutex> lock(m_);if(ready_!=value){++epoch_;result_={};requested_=false;}ready_=value;}
    uint32_t captures(){std::lock_guard<std::mutex> lock(m_);return sequence_;}
    Result request(){std::lock_guard<std::mutex> lock(m_);if(!ready_ || !supported_)return {};requested_=true;return result_;}
    uint64_t takeRequest(){std::lock_guard<std::mutex> lock(m_);auto now=std::chrono::steady_clock::now();
        if(!ready_ || !supported_ || !requested_ || now<next_)return 0;
        requested_=false;next_=now+std::chrono::milliseconds(200);return epoch_;}
    void publish(uint64_t epoch,uint32_t w,uint32_t h,std::vector<uint32_t> pixels){
        if(!w || !h || w>320 || h>180 || pixels.size()!=(size_t)w*h)return;
        std::lock_guard<std::mutex> lock(m_);if(!ready_ || !supported_ || epoch!=epoch_)return;
        result_={w,h,++sequence_,std::move(pixels)};
    }
 private:
    std::mutex m_;bool ready_=false,supported_=false,requested_=false;uint64_t epoch_=1;uint32_t sequence_=0;
    Result result_;std::chrono::steady_clock::time_point next_{};
};
}
