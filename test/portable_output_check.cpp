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
using Clock=std::chrono::steady_clock;
struct Image {std::vector<unsigned char> data;uint64_t hash=1469598103934665603ull;};
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
 // Rate cells (test/windows_rate_cell.ps1): run for a fixed measured duration,
 // count arrivals over the frame budget after a warm-up, and verify pixels on
 // every Nth output frame. Unset, the checker behaves exactly as before.
 const unsigned seconds=std::getenv("VKR_CHECK_SECONDS")?positiveEnv("VKR_CHECK_SECONDS",1):0;
 const unsigned warmup=std::getenv("VKR_CHECK_WARMUP")?positiveEnv("VKR_CHECK_WARMUP",1):0;
 const double budgetMs=std::getenv("VKR_CHECK_BUDGET_MS")?positiveEnv("VKR_CHECK_BUDGET_MS",1):0;
 const unsigned every=positiveEnv("VKR_CHECK_VERIFY_EVERY",1);
 std::vector<double> arrival[2];Clock::time_point previous[2]{};
 uint64_t received[2]{},sequenceGaps[2]{},lastSequence[2]{},misses[2]{};double measuredMax[2]{};
 const auto started=Clock::now();
 Clock::time_point measureFrom{},measureUntil{};
 for(unsigned p=0;p<2;p++)if(flCreateChannelEx((prefix+(p?".nv12":".rgba")).c_str(),w,h,p?FL_FORMAT_NV12:rgbFormat,4,FL_MAP_CPU|((std::getenv("VKR_OUTPUT_MODE") && std::string(std::getenv("VKR_OUTPUT_MODE"))=="gpu")?FL_GPU_RENDER:0),&ch[p])!=FL_OK)return 2;
 std::map<int64_t,Image> saved[2];std::set<uint64_t> hashes;unsigned matched=0;int worst=0;
 auto deadline=Clock::now()+std::chrono::seconds(timeout);
 const auto running=[&]{
  if(!seconds)return Clock::now()<deadline && matched<target;
  // Duration mode: the measured window opens at the first frame plus warm-up.
  if(measureFrom==Clock::time_point{})return Clock::now()<deadline;
  return Clock::now()<measureUntil;
 };
 while(running()){
  for(unsigned p=0;p<2;p++){
   flFrame f{};flResult r=flAcquireFrame(ch[p],&f,10);
   if(r==FL_TIMEOUT || r==FL_DISCONNECTED)continue;
   if(r!=FL_OK)return 3;
   auto now=Clock::now();
   if(seconds && measureFrom==Clock::time_point{}){
    measureFrom=now+std::chrono::seconds(warmup);measureUntil=measureFrom+std::chrono::seconds(seconds);
   }
   const bool measured=!seconds || now>=measureFrom;
   if(received[p] && measured){
    const double ms=std::chrono::duration<double,std::milli>(now-previous[p]).count();
    arrival[p].push_back(ms);measuredMax[p]=std::max(measuredMax[p],ms);
    if(budgetMs && ms>budgetMs)misses[p]++;
   }
   previous[p]=now;
   if(measured){
    received[p]++;
    if(lastSequence[p] && f.sequence>lastSequence[p]+1)sequenceGaps[p]+=f.sequence-lastSequence[p]-1;
   }
   lastSequence[p]=f.sequence;
   if(every>1 && f.sequence%every){if(flRelease(ch[p],&f)!=FL_OK)return 6;continue;}
   flMappedImage m{};if(flImageMapPlanes(f.image,&m)!=FL_OK)return 4;
   Image image;
   for(unsigned plane=0;plane<m.planes;plane++)for(unsigned y=0;y<m.plane[plane].rows;y++){
    auto* row=(unsigned char*)m.plane[plane].pixels+y*m.plane[plane].stride;
    image.data.insert(image.data.end(),row,row+m.plane[plane].rowBytes);
   }
   if(p==0 && rgbFormat==FL_FORMAT_RGB10A2)for(size_t i=0;i+3<image.data.size();i+=4){
    uint32_t word;std::memcpy(&word,image.data.data()+i,4);
    image.data[i]=(word&1023)*255/1023;image.data[i+1]=((word>>10)&1023)*255/1023;
    image.data[i+2]=((word>>20)&1023)*255/1023;image.data[i+3]=(word>>30)*85;
   }
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
   hashes.insert(it->second.hash);matched++;saved[1].erase(other);it=saved[0].erase(it);
  }
 }
 for(auto c:ch)flClose(c);
 printf("matched=%u distinct=%zu worst_YUV_error=%d (limit=3)\n",matched,hashes.size(),worst);
 if(seconds)printf("rate seconds=%u warmup=%u budget_ms=%.0f verify_every=%u misses_rgb=%llu misses_nv12=%llu max_rgb=%.3f max_nv12=%.3f\n",
  seconds,warmup,budgetMs,every,(unsigned long long)misses[0],(unsigned long long)misses[1],measuredMax[0],measuredMax[1]);
 const double elapsed=std::chrono::duration<double>(Clock::now()-started).count();
 for(unsigned p=0;p<2;p++){
  auto& a=arrival[p];std::sort(a.begin(),a.end());
  auto percentile=[&](double q){return a.empty()?0.:a[(size_t)(q*(a.size()-1))];};
  printf("channel=%s received=%llu sequence_gaps=%llu elapsed_s=%.3f arrival_ms_p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",p?"nv12":"rgb",(unsigned long long)received[p],(unsigned long long)sequenceGaps[p],elapsed,percentile(.5),percentile(.95),percentile(.99),percentile(1));
 }
 const bool pixels=matched>=target && hashes.size()>=std::min(target,10u) && worst<=3;
 if(!seconds)return pixels?0:1;
 // A rate cell also needs the whole window: no budget misses, no sequence gaps,
 // and frames on both channels.
 const bool window=Clock::now()>=measureUntil && measureFrom!=Clock::time_point{};
 const bool rate=!misses[0] && !misses[1] && !sequenceGaps[0] && !sequenceGaps[1] && received[0] && received[1];
 return pixels && window && rate?0:1;
}
