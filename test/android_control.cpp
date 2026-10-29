#include "android_control.h"
#include <thread>
#include <chrono>
#include <cstdio>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 vkr::AndroidControl c;
 CHECK(c.submit("pause",-1,"1")==c.Unavailable);
 CHECK(c.submit("camera",-1,"1")==c.Unsupported);
 CHECK(c.submit("layout",-1,"wrong")==c.Invalid);
 CHECK(c.submit("rotate_seconds",-1,"nan")==c.Invalid);
 CHECK(c.submit("max_visible",-1,"25")==c.Invalid);
 c.publish("{}",{0,12},0);c.setReady(true);
 CHECK(c.submit("hide",13,"")==c.Invalid);
 CHECK(c.submit("hide",12,"")==c.Queued);
 CHECK(c.submit("pause",-1,"1")==c.Queued);
 auto queue=c.drain();CHECK(queue.size()==2);CHECK(queue[0].slot==12);CHECK(queue[1].i==1);
 for(const char* action:{"hide","show","pin","unpin","spotlight","unspotlight","front","kick"})CHECK(c.submit(action,12,"")==c.Queued);
 CHECK(c.submit("max_visible",-1,"2")==c.Queued);CHECK(c.submit("rotate_seconds",-1,"3.5")==c.Queued);
 for(const char* layout:{"wall","presentation","two-cubes","single-cube"})CHECK(c.submit("layout",-1,layout)==c.Queued);
 CHECK(c.drain().size()==14);
 for(unsigned i=0;i<128;i++)CHECK(c.submit("pause",-1,"1")==c.Queued);
 CHECK(c.submit("pause",-1,"0")==c.Full);
 c.setReady(false);CHECK(c.drain().empty());CHECK(c.status().find("\"ready\":false")!=std::string::npos);
 // A paused or destroyed renderer never requires a render-thread response.
 auto start=std::chrono::steady_clock::now();
 for(unsigned i=0;i<10000;i++){CHECK(c.submit("pause",-1,"1")==c.Unavailable);CHECK(!c.status().empty());}
 CHECK(std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
 return 0;
}
