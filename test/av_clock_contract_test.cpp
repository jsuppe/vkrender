// Independent scheduler fixtures. These validate policy, not device playback or
// physical lip sync. Include the production scheduler from the integration tree.
#include "av_schedule.h"
#include <gst/gst.h>
#include <gst/check/gsttestclock.h>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(int argc,char**argv){
 gst_init(&argc,&argv);using D=vkr::AvSchedule::Decision;
 GstSegment video,audio;gst_segment_init(&video,GST_FORMAT_TIME);gst_segment_init(&audio,GST_FORMAT_TIME);
 video.start=5*GST_SECOND;video.base=3*GST_SECOND;video.time=2*GST_SECOND;
 audio.start=9*GST_SECOND;audio.base=GST_SECOND;audio.time=8*GST_SECOND;
 auto vp=gst_segment_to_running_time(&video,GST_FORMAT_TIME,6*GST_SECOND);
 auto ap=gst_segment_to_running_time(&audio,GST_FORMAT_TIME,12*GST_SECOND);
 CHECK(vp==4*GST_SECOND && ap==vp);
 CHECK(gst_segment_to_running_time(&video,GST_FORMAT_TIME,4*GST_SECOND)==GST_CLOCK_TIME_NONE);
 const uint64_t base=70*GST_SECOND;
 auto* master=gst_test_clock_new_with_start_time(base);
 auto* unrelated=gst_test_clock_new_with_start_time(base+99*GST_SECOND);
 vkr::AvSchedule s;
 // Hardware latency fixtures move *both* deadlines. Audio's expected deadline
 // is computed independently here, not with the production scheduler.
 for(uint64_t latency: {uint64_t(0),uint64_t(20*GST_MSECOND),uint64_t(100*GST_MSECOND)}){
  uint64_t audioDeadline=base+ap+latency;
  gst_test_clock_set_time(GST_TEST_CLOCK(master),audioDeadline-1);
  CHECK(s.decide(vp,gst_clock_get_time(master),base,latency)==D::Hold);
  gst_test_clock_set_time(GST_TEST_CLOCK(master),audioDeadline);
  CHECK(s.decide(vp,gst_clock_get_time(master),base,latency)==D::Present);
  CHECK(s.lastErrorNs==0);
  CHECK(s.decide(vp,gst_clock_get_time(unrelated),base,latency)==D::Drop);
 }
 // No source/arrival clock can change a deadline; only selected master time.
 // Emulate +/-1000ppm over 10s, proving decisions follow master not wall time.
 for(int ppm: {-1000,1000}){
  for(int i=0;i<=1000;++i){
   int64_t pts=int64_t(i)*10*GST_MSECOND;
   int64_t offset=pts*ppm/1000000;
   uint64_t now=base+uint64_t(pts+offset);
   auto d=s.decide(pts,now,base,0);
   CHECK(d==(offset<0?D::Hold:D::Present));
   CHECK(s.lastErrorNs==offset);
  }
 }
 CHECK(s.decide(0,base+50*GST_MSECOND,base,0)==D::Present);
 CHECK(s.decide(0,base+50*GST_MSECOND+1,base,0)==D::Drop);
 // Pause clock held: new wall-clock arrival does not advance playback.
 for(int i=0;i<100;++i)CHECK(s.decide(GST_SECOND,base,base,0)==D::Hold);
 CHECK(s.decide(GST_SECOND,base+GST_SECOND,base,0)==D::Present);
 s.reset(42);CHECK(s.generation==42 && s.lastErrorNs==0);
 CHECK(s.decide(0,base+100*GST_SECOND,base+100*GST_SECOND,0)==D::Present);
 CHECK(s.decide(INT64_MAX,0,UINT64_MAX-1,1)==D::Invalid);
 gst_object_unref(master);gst_object_unref(unrelated);
 std::puts("Independent segment/shared-epoch/latency/clock-drift/late-boundary/pause policy fixtures passed; physical playback not tested");
}
