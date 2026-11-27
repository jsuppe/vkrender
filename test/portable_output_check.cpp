#include "framelink.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
using Clock=std::chrono::steady_clock;
struct Image {std::vector<unsigned char> data;uint64_t hash=1469598103934665603ull;};
// Worst BT.709 limited-range error between an RGB(A) frame and its NV12 twin.
static int pairError(const std::vector<unsigned char>& rgb,const std::vector<unsigned char>& nv,unsigned w,unsigned h){
 int worst=0;
 for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
  const auto* px=&rgb[(y*w+x)*4];
  int expected=(int)std::lround(16+(0.2126*px[0]+0.7152*px[1]+0.0722*px[2])*219/255);
  worst=std::max(worst,std::abs(expected-nv[y*w+x]));
  if(!(x&1)&&!(y&1)){
   double r=0,g=0,b=0;
   for(unsigned dy=0;dy<2;dy++)for(unsigned dx=0;dx<2;dx++){
    const auto* q=&rgb[((y+dy)*w+x+dx)*4];r+=q[0]/4.;g+=q[1]/4.;b+=q[2]/4.;
   }
   int u=(int)std::lround(128+(-0.114572*r-0.385428*g+0.5*b)*224/255);
   int v=(int)std::lround(128+(0.5*r-0.454153*g-0.045847*b)*224/255);
   worst=std::max(worst,std::abs(u-nv[w*h+y/2*w+x]));
   worst=std::max(worst,std::abs(v-nv[w*h+y/2*w+x+1]));
  }
 }
 return worst;
}
static Image copyImage(const flMappedImage& m,bool rgb10){
 Image image;
 for(unsigned plane=0;plane<m.planes;plane++)for(unsigned y=0;y<m.plane[plane].rows;y++){
  auto* row=(unsigned char*)m.plane[plane].pixels+y*m.plane[plane].stride;
  image.data.insert(image.data.end(),row,row+m.plane[plane].rowBytes);
 }
 if(rgb10)for(size_t i=0;i+3<image.data.size();i+=4){
  uint32_t word;std::memcpy(&word,image.data.data()+i,4);
  image.data[i]=(word&1023)*255/1023;image.data[i+1]=((word>>10)&1023)*255/1023;
  image.data[i+2]=((word>>20)&1023)*255/1023;image.data[i+3]=(word>>30)*85;
 }
 return image;
}
static uint64_t mix(uint64_t x){x+=0x9e3779b97f4a7c15ull;x=(x^(x>>30))*0xbf58476d1ce4e5b9ull;x=(x^(x>>27))*0x94d049bb133111ebull;return x^(x>>31);}
static int rateCell(const std::string& prefix,flChannel** ch,flFormat rgbFormat,unsigned w,unsigned h,unsigned target,unsigned timeout);
int main(int argc,char** argv){
 const std::string prefix=argc>1?argv[1]:"parity.output";
 flChannel* ch[2]{};
 const flFormat rgbFormat=argc>2 && std::string(argv[2])=="rgb10a2"?FL_FORMAT_RGB10A2:FL_FORMAT_RGBA8;
 const auto positiveEnv=[](const char* name,unsigned fallback){
  const char* value=std::getenv(name);if(!value)return fallback;
  char* end=nullptr;unsigned long n=std::strtoul(value,&end,10);
  if(!*value || *end || !n || n>1000000){fprintf(stderr,"invalid %s\n",name);std::exit(2);}
  return (unsigned)n;
 };
 const unsigned w=positiveEnv("VKR_CHECK_WIDTH",160),h=positiveEnv("VKR_CHECK_HEIGHT",90);
 if(w>8192 || h>8192 || (w&1) || (h&1)){fprintf(stderr,"invalid even dimensions\n");return 2;}
 const unsigned target=positiveEnv("VKR_CHECK_FRAMES",20);
 const unsigned timeout=positiveEnv("VKR_CHECK_TIMEOUT",45);
 std::vector<double> arrival[2];Clock::time_point previous[2]{};
 uint64_t received[2]{},sequenceGaps[2]{},lastSequence[2]{};
 const auto started=Clock::now();
 const bool gpuMode=std::getenv("VKR_OUTPUT_MODE") && std::string(std::getenv("VKR_OUTPUT_MODE"))=="gpu";
 // A rate cell that only times GPU outputs (VKR_CHECK_VERIFY_EVERY=0) is a GPU
 // consumer: a CPU-mappable ring would add a readback per frame on the checker's side.
 const bool timingOnly=gpuMode && std::getenv("VKR_CHECK_SECONDS") && std::getenv("VKR_CHECK_VERIFY_EVERY") && std::string(std::getenv("VKR_CHECK_VERIFY_EVERY"))=="0";
 // FL_VULKAN_IMPORT: the renderer writes these rings through Vulkan; without it NVIDIA
 // Linux allocates a compressed layout that only EGL can import.
 const uint32_t flags=(timingOnly?FL_VULKAN_IMPORT:FL_MAP_CPU)|(gpuMode?FL_GPU_RENDER:0u);
 for(unsigned p=0;p<2;p++)if(flCreateChannelEx((prefix+(p?".nv12":".rgba")).c_str(),w,h,p?FL_FORMAT_NV12:rgbFormat,4,flags,&ch[p])!=FL_OK)return 2;
 if(std::getenv("VKR_CHECK_SECONDS"))return rateCell(prefix,ch,rgbFormat,w,h,target,timeout);
 std::map<int64_t,Image> saved[2];std::set<uint64_t> hashes;unsigned matched=0;int worst=0;
 auto deadline=Clock::now()+std::chrono::seconds(timeout);
 while(Clock::now()<deadline && matched<target){
  for(unsigned p=0;p<2;p++){
   flFrame f{};flResult r=flAcquireFrame(ch[p],&f,10);
   if(r==FL_TIMEOUT || r==FL_DISCONNECTED)continue;
   if(r!=FL_OK)return 3;
   auto now=Clock::now();
   if(received[p])arrival[p].push_back(std::chrono::duration<double,std::milli>(now-previous[p]).count());
   previous[p]=now;received[p]++;
   if(lastSequence[p] && f.sequence>lastSequence[p]+1)sequenceGaps[p]+=f.sequence-lastSequence[p]-1;
   lastSequence[p]=f.sequence;
   flMappedImage m{};if(flImageMapPlanes(f.image,&m)!=FL_OK)return 4;
   Image image=copyImage(m,p==0 && rgbFormat==FL_FORMAT_RGB10A2);
   for(auto c:image.data){image.hash^=c;image.hash*=1099511628211ull;}
   flImageUnmap(f.image);if(flRelease(ch[p],&f)!=FL_OK)return 6;
   saved[p][f.ptsNs]=std::move(image);
   if(saved[p].size()>64)saved[p].erase(saved[p].begin());
  }
  for(auto it=saved[0].begin();it!=saved[0].end();){
   auto other=saved[1].find(it->first);if(other==saved[1].end()){++it;continue;}
   const auto& rgb=it->second.data;const auto& nv=other->second.data;
   if(matched==0 && std::getenv("VKR_OUTPUT_DUMP")){
    FILE* r=fopen("gpu-check.rgba","wb");if(r){fwrite(rgb.data(),1,rgb.size(),r);fclose(r);}
    FILE* n=fopen("gpu-check.nv12","wb");if(n){fwrite(nv.data(),1,nv.size(),n);fclose(n);}
   }
   if(rgb.size()!=w*h*4 || nv.size()!=w*h*3/2)return 5;
   worst=std::max(worst,pairError(rgb,nv,w,h));
   hashes.insert(it->second.hash);matched++;saved[1].erase(other);it=saved[0].erase(it);
  }
 }
 for(auto c:ch)flClose(c);
 printf("matched=%u distinct=%zu worst_YUV_error=%d (limit=3)\n",matched,hashes.size(),worst);
 const double seconds=std::chrono::duration<double>(Clock::now()-started).count();
 for(unsigned p=0;p<2;p++){
  auto& a=arrival[p];std::sort(a.begin(),a.end());
  auto percentile=[&](double q){return a.empty()?0.:a[(size_t)(q*(a.size()-1))];};
  printf("channel=%s received=%llu sequence_gaps=%llu elapsed_s=%.3f arrival_ms_p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",p?"nv12":"rgb",(unsigned long long)received[p],(unsigned long long)sequenceGaps[p],seconds,percentile(.5),percentile(.95),percentile(.99),percentile(1));
 }
 return matched>=target && hashes.size()>=std::min(target,10u) && worst<=3?0:1;
}

// Rate cell (VKR_CHECK_SECONDS set; test/windows_rate_cell.ps1, test/linux_rate_cell.py).
// One acquire thread per channel times arrivals and holds frames only briefly.
// Every Nth scene frame, chosen by a hash of its PTS so both channels pick the
// same one, is copied out and compared on a separate verify thread. The
// measured window opens at the first frame plus VKR_CHECK_WARMUP seconds.
static int rateCell(const std::string& prefix,flChannel** ch,flFormat rgbFormat,unsigned w,unsigned h,unsigned target,unsigned timeout){
 const auto number=[](const char* name,unsigned fallback){
  const char* value=std::getenv(name);if(!value)return fallback;
  char* end=nullptr;unsigned long n=std::strtoul(value,&end,10);
  if(!*value || *end || n>1000000){fprintf(stderr,"invalid %s\n",name);std::exit(2);}
  return (unsigned)n;
 };
 const unsigned seconds=number("VKR_CHECK_SECONDS",0),warmup=number("VKR_CHECK_WARMUP",0);
 // Budget: per-frame latency, arrival minus the frame's PTS (the renderer
 // stamps scene outputs with steady_clock at record), as FrameLink's arrival
 // gate. Stall: longest allowed gap between consecutive output frames.
 const unsigned budgetMs=number("VKR_CHECK_BUDGET_MS",0),stallMs=number("VKR_CHECK_STALL_MS",0);
 const unsigned every=number("VKR_CHECK_VERIFY_EVERY",1); // 0: timing only, no pixel copies
 if(!seconds){fprintf(stderr,"invalid VKR_CHECK_SECONDS\n");return 2;}
 std::atomic<int64_t> openNs{0};std::atomic<bool> stop{false};std::atomic<int> error{0};
 const auto startNs=Clock::now().time_since_epoch().count();
 struct Stats{std::vector<double> arrival,latency;uint64_t received=0,gaps=0,misses=0,stalls=0,sampled=0,unstamped=0;double max=0;};
 Stats stats[2];
 std::mutex lock;std::condition_variable wake;std::map<int64_t,Image> saved[2];
 std::deque<std::pair<Image,Image>> pairs;uint64_t droppedSamples=0;
 auto acquire=[&](unsigned p){
  Clock::time_point previous{};uint64_t lastSequence=0;bool first=true;auto& st=stats[p];
  st.arrival.reserve(size_t(seconds)*64);st.latency.reserve(size_t(seconds)*64);
  while(!stop){
   flFrame f{};flResult r=flAcquireFrame(ch[p],&f,10);
   if(r==FL_TIMEOUT || r==FL_DISCONNECTED)continue;
   if(r!=FL_OK){error=3;stop=true;return;}
   const auto now=Clock::now();
   int64_t expected=0;
   if(openNs.compare_exchange_strong(expected,(now+std::chrono::seconds(warmup)).time_since_epoch().count()))wake.notify_all();
   const bool measured=now.time_since_epoch().count()>=openNs.load();
   if(measured){
    if(!first){
     const double ms=std::chrono::duration<double,std::milli>(now-previous).count();
     st.arrival.push_back(ms);st.max=std::max(st.max,ms);if(stallMs && ms>stallMs)st.stalls++;
     if(f.sequence>lastSequence+1)st.gaps+=f.sequence-lastSequence-1;
    }
    if(f.ptsNs>0){
     const double lat=double(now.time_since_epoch().count()-f.ptsNs)*1e-6;
     st.latency.push_back(lat);if(budgetMs && lat>budgetMs)st.misses++;
    } else st.unstamped++;
    st.received++;first=false;
   }
   previous=now;lastSequence=f.sequence;
   if(measured && every && mix((uint64_t)f.ptsNs)%every==0){
    flMappedImage m{};if(flImageMapPlanes(f.image,&m)!=FL_OK){error=4;stop=true;return;}
    Image image=copyImage(m,p==0 && rgbFormat==FL_FORMAT_RGB10A2);
    flImageUnmap(f.image);st.sampled++;
    std::lock_guard<std::mutex> g(lock);
    auto other=saved[1-p].find(f.ptsNs);
    if(other!=saved[1-p].end()){
     pairs.emplace_back(p?std::move(other->second):std::move(image),p?std::move(image):std::move(other->second));
     saved[1-p].erase(other);
     if(pairs.size()>4){pairs.pop_front();droppedSamples++;} // verify is behind; never block acquisition
     wake.notify_all();
    } else {
     saved[p][f.ptsNs]=std::move(image);
     while(saved[p].size()>8)saved[p].erase(saved[p].begin());
    }
   }
   if(flRelease(ch[p],&f)!=FL_OK){error=6;stop=true;return;}
  }
 };
 unsigned matched=0;int worst=0;std::set<uint64_t> hashes;
 std::thread verify([&]{
  for(;;){
   std::pair<Image,Image> pair;
   {std::unique_lock<std::mutex> g(lock);wake.wait(g,[&]{return stop || !pairs.empty();});
    if(pairs.empty())return;pair=std::move(pairs.front());pairs.pop_front();}
   if(pair.first.data.size()!=size_t(w)*h*4 || pair.second.data.size()!=size_t(w)*h*3/2){error=5;stop=true;wake.notify_all();return;}
   const int e=pairError(pair.first.data,pair.second.data,w,h);
   for(auto c:pair.first.data){pair.first.hash^=c;pair.first.hash*=1099511628211ull;}
   std::lock_guard<std::mutex> g(lock);worst=std::max(worst,e);hashes.insert(pair.first.hash);matched++;
  }
 });
 std::thread a0(acquire,0u),a1(acquire,1u);
 const auto deadline=Clock::now()+std::chrono::seconds(timeout);
 bool complete=false;
 while(!stop && Clock::now()<deadline){
  const int64_t open=openNs.load();
  if(open && Clock::now().time_since_epoch().count()>=open+int64_t(seconds)*1000000000ll){complete=true;break;}
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
 }
 stop=true;wake.notify_all();a0.join();a1.join();verify.join();
 for(unsigned p=0;p<2;p++)flClose(ch[p]);
 printf("matched=%u distinct=%zu worst_YUV_error=%d (limit=3)\n",matched,hashes.size(),worst);
 printf("rate seconds=%u warmup=%u budget_ms=%u stall_ms=%u verify_every=%u complete=%d samples_dropped=%llu misses_rgb=%llu misses_nv12=%llu stalls_rgb=%llu stalls_nv12=%llu unstamped=%llu\n",
  seconds,warmup,budgetMs,stallMs,every,complete?1:0,(unsigned long long)droppedSamples,
  (unsigned long long)stats[0].misses,(unsigned long long)stats[1].misses,(unsigned long long)stats[0].stalls,(unsigned long long)stats[1].stalls,
  (unsigned long long)(stats[0].unstamped+stats[1].unstamped));
 const double elapsed=double(Clock::now().time_since_epoch().count()-startNs)*1e-9;
 for(unsigned p=0;p<2;p++){
  auto& a=stats[p].arrival;auto& l=stats[p].latency;std::sort(a.begin(),a.end());std::sort(l.begin(),l.end());
  auto pct=[](const std::vector<double>& v,double q){return v.empty()?0.:v[(size_t)(q*(v.size()-1))];};
  printf("channel=%s received=%llu sequence_gaps=%llu elapsed_s=%.3f latency_ms_p50=%.3f p95=%.3f p99=%.3f max=%.3f interval_ms_p50=%.3f p99=%.3f max=%.3f\n",p?"nv12":"rgb",
   (unsigned long long)stats[p].received,(unsigned long long)stats[p].gaps,elapsed,pct(l,.5),pct(l,.95),pct(l,.99),pct(l,1),pct(a,.5),pct(a,.99),pct(a,1));
 }
 if(error)return error;
 const bool pixels=!every || (matched>=target && hashes.size()>=std::min(target,10u) && worst<=3);
 const bool rate=complete && !stats[0].misses && !stats[1].misses && !stats[0].stalls && !stats[1].stalls && !stats[0].gaps && !stats[1].gaps
  && !stats[0].unstamped && !stats[1].unstamped && stats[0].received && stats[1].received;
 return pixels && rate?0:1;
}
