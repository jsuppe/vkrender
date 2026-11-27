#pragma once
#include "av_playback.h"
#include "av_schedule.h"
#include <thread>
#include <chrono>
#include <stdexcept>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstdio>
#include <cstdlib>
namespace vkr {
// One selected A/V session occupies slot zero. Ordinary slots remain low-latency.
// Receiver never calls Vulkan: boundary work is acknowledged by render thread.
class AvInput {
public:
 struct Request {flSessionConfig config{};std::string name;bool end=false;bool prepare=false;std::chrono::steady_clock::time_point deadline{};};
 // An end request waits, while frames are still pending, until this time:
 // they are presented at their due times before the generation ends.
 std::chrono::steady_clock::time_point drainUntil_{};
 std::atomic<bool> stop{false}, muted{false}, paused{false}, finished{false}, succeeded{false};
 AvSchedule schedule;
 explicit AvInput(av::PlaybackOptions options,bool nativePreparation=false):nativePreparation_(nativePreparation),options_(options) {
  ringSlots_=AvRingBudget::testRingSlots(std::getenv("VKR_TEST_AV_POOL_SLOTS"));
  start(options);
 }
 unsigned ringSlots() const{return ringSlots_;}
 bool configured() const{return !options_.plugin.empty();}
 bool done() const{return finished.load() && !switching_;}
 void selectSession(const std::string& session){
  if(!configured() || session.empty())return;
  switching_=true;nextSession_=session;stop=true;cv_.notify_all();
 }
 void start(av::PlaybackOptions options) {
  stop=false;finished=false;succeeded=false;
  options.mute=&muted;options.paused=&paused;
  if(options.session.empty())return;
  worker_=std::thread([this,options]{
   av::PlaybackCallbacks cb;
   cb.onSession=[this](const flSessionConfig& c,const std::string& n,const av::PlaybackClock& clock){
    std::unique_lock<std::mutex> l(m_);
    if(clock_)gst_object_unref(clock_);clock_=clock.clock?GST_CLOCK(gst_object_ref(clock.clock)):nullptr;
    base_=clock.baseTimeNs;latency_=clock.latencyNs;audio_=clock.audioMaster;
    budgetNs_=AvRingBudget::maxLatencyNs(c.video.fpsNum,c.video.fpsDen);
    reportBudget();
    metrics_={};metricsValid_=false;
    request_=std::make_unique<Request>(Request{c,n,false});
    cv_.wait(l,[this]{return !request_||stop.load();});
   };
   if(nativePreparation_ && options.clockPolicy==av::PlaybackClockPolicy::ProducerMapped)cb.onPrepare=[this](unsigned remainingMs){
    std::unique_lock<std::mutex> l(m_);
    if(stop.load())throw std::runtime_error("native preparation cancelled");
    prepareError_.clear();request_=std::make_unique<Request>();request_->prepare=true;
    request_->deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(remainingMs);
    const auto deadline=request_->deadline;
    if(!cv_.wait_until(l,deadline,[this]{return !request_||stop.load();})){
      stop=true;throw std::runtime_error("native preparation deadline");
    }
    if(stop.load())throw std::runtime_error("native preparation cancelled");
    if(!prepareError_.empty()){stop=true;throw std::runtime_error(prepareError_);}
   };
   cb.onClock=[this](const av::PlaybackClock& clock){std::lock_guard<std::mutex> l(m_);
    if(clock_)gst_object_unref(clock_);clock_=clock.clock?GST_CLOCK(gst_object_ref(clock.clock)):nullptr;
   base_=clock.baseTimeNs;latency_=clock.latencyNs;audio_=clock.audioMaster;
    // onSession precedes the selected playback clock; report only its real
    // negotiated latency here, not the empty startup placeholder.
    std::fprintf(stderr,"AV_RING_CONFIG ring_slots=%u pending_slots=%u common_latency_ns=%llu\n",
     ringSlots_,AvRingBudget::pendingSlots,(unsigned long long)latency_);
    reportBudget();
   };
   // The clock is kept until the render thread services the end, so frames
   // still pending at the producer's end are presented at their due times
   // (6a, 2026-11-27: the final frame was discarded when the end overtook it).
   // Bounded: common latency plus the pending budget plus one frame.
   cb.onEnd=[this]{std::unique_lock<std::mutex> l(m_);if(stop)return;
    request_=std::make_unique<Request>();request_->end=true;endServiceCalls_=0;
    const uint64_t frameNs=budgetNs_/AvRingBudget::pendingSlots;
    drainUntil_=std::chrono::steady_clock::now()+std::chrono::nanoseconds(latency_+budgetNs_+frameNs);
    cv_.wait(l,[this]{return !request_||stop.load();});};
   cb.onMetrics=[this](const av::PlaybackMetrics& m){std::lock_guard<std::mutex> l(m_);metrics_=m;metricsValid_=true;};
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
 // pendingFrames: the render thread still holds scheduled frames for this
 // session; an end request then waits for them (bounded by drainUntil_).
 template<class F> void service(F fn,bool pendingFrames=false) {
  std::unique_lock<std::mutex> l(m_);
  if(switching_ && (finished.load() || !worker_.joinable())){
   if(worker_.joinable())worker_.join();
   Request end{};end.end=true;fn(end);schedule.reset(0);active_=false;
   if(clock_){gst_object_unref(clock_);clock_=nullptr;}audio_=false;request_.reset();
   options_.session=nextSession_;nextSession_.clear();switching_=false;
   status_="connecting";l.unlock();start(options_);return;
  }
  if(!request_)return;
  // service() runs before the input poll in each render iteration, so an end
  // is held for at least one poll after it arrived: a final frame can still
  // be unread in FrameLink's queue when the end overtakes it.
  if(request_->end && active_ && std::chrono::steady_clock::now()<drainUntil_ && (pendingFrames || ++endServiceCalls_<2))return;
  if(request_->end){
   if(pendingFrames)std::fprintf(stderr,"AV_END_DRAIN pending frames remained at the drain bound\n");
   if(clock_){gst_object_unref(clock_);clock_=nullptr;}audio_=false;
  }
  if(request_->prepare){
   try{fn(*request_);}catch(const std::exception& e){prepareError_=e.what();}
   catch(...){prepareError_="native preparation failed";}
  }else{fn(*request_);schedule.reset(request_->config.generation);active_=!request_->end;}
  request_.reset();cv_.notify_all();
 }
 bool active() const{return active_;}
 AvSchedule::Decision decide(int64_t pts){std::lock_guard<std::mutex> l(m_);
  if(paused.load() || !clock_)return AvSchedule::Decision::Hold;
  return schedule.decide(pts,gst_clock_get_time(clock_),base_,latency_);
 }
 std::string status(){std::lock_guard<std::mutex> l(m_);return status_;}
 struct MetricsSnapshot {av::PlaybackMetrics audio;bool valid=false;};
 MetricsSnapshot metrics(){std::lock_guard<std::mutex> l(m_);return {metrics_,metricsValid_};}
 bool audioMaster(){std::lock_guard<std::mutex> l(m_);return audio_;}
private:
 // Caller holds m_. Report negotiated latency; never lower the common A/V epoch.
 void reportBudget(){
  if(latency_>budgetNs_ && (!budgetWarned_ || warnedLatency_!=latency_)){
   std::fprintf(stderr,"AV_RING_BUDGET latency_ns=%llu nominal_max_ns=%llu ring_slots=%u pending_slots=%u policy=bounded_backpressure_drops_possible\n",
    (unsigned long long)latency_,(unsigned long long)budgetNs_,ringSlots_,AvRingBudget::pendingSlots);
   budgetWarned_=true;warnedLatency_=latency_;
  }
 }
 uint64_t budgetNs_=0,warnedLatency_=0;bool budgetWarned_=false;unsigned endServiceCalls_=0;
 std::string prepareError_;
 bool nativePreparation_=false;
 unsigned ringSlots_=AvRingBudget::ringSlots;
 av::PlaybackMetrics metrics_{};bool metricsValid_=false;
 av::PlaybackOptions options_;bool switching_=false;std::string nextSession_;
 std::thread worker_;std::mutex m_;std::condition_variable cv_;std::unique_ptr<Request> request_;
 GstClock* clock_=nullptr;uint64_t base_=0,latency_=0;bool active_=false,audio_=false;std::string status_="connecting";
};
}
