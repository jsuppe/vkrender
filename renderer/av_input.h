#pragma once
#include "av_playback.h"
#include "av_schedule.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstdio>
namespace vkr {
// One selected A/V session occupies slot zero. Ordinary slots remain low-latency.
// Receiver never calls Vulkan: boundary work is acknowledged by render thread.
class AvInput {
public:
 struct Request {flSessionConfig config{};std::string name;bool end=false;};
 std::atomic<bool> stop{false}, muted{false}, paused{false}, finished{false}, succeeded{false};
 AvSchedule schedule;
 explicit AvInput(av::PlaybackOptions options) {
  options.mute=&muted;options.paused=&paused;
  if(options.session.empty())return;
  worker_=std::thread([this,options]{
   av::PlaybackCallbacks cb;
   cb.onSession=[this](const flSessionConfig& c,const std::string& n,const av::PlaybackClock& clock){
    std::unique_lock<std::mutex> l(m_);
    if(clock_)gst_object_unref(clock_);clock_=clock.clock?GST_CLOCK(gst_object_ref(clock.clock)):nullptr;
    base_=clock.baseTimeNs;latency_=clock.latencyNs;audio_=clock.audioMaster;
    request_=std::make_unique<Request>(Request{c,n,false});
    cv_.wait(l,[this]{return !request_||stop.load();});
   };
   cb.onClock=[this](const av::PlaybackClock& clock){std::lock_guard<std::mutex> l(m_);
    if(clock_)gst_object_unref(clock_);clock_=clock.clock?GST_CLOCK(gst_object_ref(clock.clock)):nullptr;
    base_=clock.baseTimeNs;latency_=clock.latencyNs;audio_=clock.audioMaster;
   };
   cb.onEnd=[this]{std::unique_lock<std::mutex> l(m_);if(stop)return;
    if(clock_){gst_object_unref(clock_);clock_=nullptr;}audio_=false;
    request_=std::make_unique<Request>();request_->end=true;
    cv_.wait(l,[this]{return !request_||stop.load();});};
   cb.onStatus=[this](const std::string& s){std::lock_guard<std::mutex> l(m_);status_=s;};
   std::string error;bool ok=av::runPlayback(options,cb,stop,error);
   std::lock_guard<std::mutex> l(m_);status_=ok?"ended":error;succeeded=ok;finished=true;
  });
 }
 ~AvInput(){shutdown();if(clock_)gst_object_unref(clock_);}
 void shutdown(){stop=true;cv_.notify_all();if(worker_.joinable()){
  worker_.join();std::fprintf(stderr,"AV_RENDER due_decisions=%llu hold_checks=%llu late_drops=%llu invalid_pts=%llu status=%s\n",
   (unsigned long long)schedule.presented,(unsigned long long)schedule.held,
   (unsigned long long)schedule.late,(unsigned long long)schedule.invalid,status().c_str());
 }}
 template<class F> void service(F fn) {
  std::lock_guard<std::mutex> l(m_);if(!request_)return;
  fn(*request_);schedule.reset(request_->config.generation);active_=!request_->end;
  request_.reset();cv_.notify_all();
 }
 bool active() const{return active_;}
 AvSchedule::Decision decide(int64_t pts){std::lock_guard<std::mutex> l(m_);
  if(paused.load() || !clock_)return AvSchedule::Decision::Hold;
  return schedule.decide(pts,gst_clock_get_time(clock_),base_,latency_);
 }
 std::string status(){std::lock_guard<std::mutex> l(m_);return status_;}
 bool audioMaster(){std::lock_guard<std::mutex> l(m_);return audio_;}
private:
 std::thread worker_;std::mutex m_;std::condition_variable cv_;std::unique_ptr<Request> request_;
 GstClock* clock_=nullptr;uint64_t base_=0,latency_=0;bool active_=false,audio_=false;std::string status_="connecting";
};
}
