#pragma once
// Binder control mailbox. No Android/Vulkan calls and no waits for render work.
#include "control_api.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
namespace vkr {
class AndroidControl {
 public:
    enum Result { Queued=0, Invalid=1, Unsupported=2, Unavailable=3, Full=4 };
    static constexpr unsigned MaxSlots=24;
    static const char* capabilities() {
        return "{\"version\":1,\"transport\":\"binder\",\"moderation\":true,\"camera\":false,\"preview\":false,\"displays\":false,\"maxSlots\":24}";
    }
    void setReady(bool value) {std::lock_guard<std::mutex> lock(mutex_);ready_=value;if(!value)pending_.clear();}
    int submit(const std::string& action,int slot,const std::string& value) {
        ControlCommand c{};c.slot=slot;
        using T=ControlCommand::Type;
        if(action=="camera" || action=="display" || action=="preview")return Unsupported;
        if(action=="layout") {
            if(value!="wall" && value!="presentation" && value!="two-cubes" && value!="single-cube")return Invalid;
            c.type=T::Layout;c.str=value;
        } else if(action=="pause") {
            if(value!="0" && value!="1")return Invalid;c.type=T::Pause;c.i=value=="1";
        } else if(action=="max_visible") {
            char* end=nullptr;long n=std::strtol(value.c_str(),&end,10);
            if(value.empty() || *end || n<1 || n>MaxSlots)return Invalid;
            c.type=T::Config;c.i=n;c.d=-1;
        } else if(action=="rotate_seconds") {
            char* end=nullptr;double n=std::strtod(value.c_str(),&end);
            if(value.empty() || *end || !std::isfinite(n) || n<1 || n>86400)return Invalid;
            c.type=T::Config;c.i=-1;c.d=n;
        } else if(action=="unspotlight")c.type=T::Unspotlight;
        else {
            if(slot<0 || slot>=(int)MaxSlots || !value.empty())return Invalid;
            if(action=="hide")c.type=T::Hide;
            else if(action=="show")c.type=T::Show;
            else if(action=="pin")c.type=T::Pin;
            else if(action=="unpin")c.type=T::Unpin;
            else if(action=="spotlight")c.type=T::Spotlight;
            else if(action=="front")c.type=T::Front;
            else if(action=="kick")c.type=T::Kick;
            else return Invalid;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if(!ready_)return Unavailable;
        if(c.type!=T::Layout && c.type!=T::Pause && c.type!=T::Config && c.type!=T::Unspotlight &&
            std::find(slots_.begin(),slots_.end(),slot)==slots_.end())return Invalid;
        if(pending_.size()>=128)return Full;
        pending_.push_back(std::move(c));return Queued;
    }
    std::vector<ControlCommand> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ControlCommand> result;result.swap(pending_);return result;
    }
    void publish(std::string state,std::vector<int> slots,uint64_t applied) {
        std::lock_guard<std::mutex> lock(mutex_);state_=std::move(state);slots_=std::move(slots);applied_=applied;
    }
    std::string status() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::string("{\"version\":1,\"ready\":")+(ready_?"true":"false")+
            ",\"pending\":"+std::to_string(pending_.size())+",\"applied\":"+std::to_string(applied_)+",\"state\":"+state_+"}";
    }
 private:
    std::mutex mutex_;bool ready_=false;uint64_t applied_=0;
    std::vector<ControlCommand> pending_;std::vector<int> slots_;
    std::string state_="{}";
};
}
