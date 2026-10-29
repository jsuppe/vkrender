#include "preview_mailbox.h"
#include <cstdio>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
 vkr::PreviewMailbox box;CHECK(box.request().argb.empty());CHECK(!box.takeRequest());
 box.supported(true);box.ready(true);CHECK(!box.takeRequest()); // no readback without request
 CHECK(box.request().argb.empty());auto epoch=box.takeRequest();CHECK(epoch);
 CHECK(!box.takeRequest());box.publish(epoch,2,1,{0xff0000ff,0xff00ff00});
 auto frame=box.request();CHECK(frame.width==2 && frame.height==1 && frame.sequence==1 && frame.argb.size()==2);
 CHECK(!box.takeRequest()); // rate-limited even with many polling callers
 box.publish(epoch,321,1,std::vector<uint32_t>(321));CHECK(box.captures()==1);
 box.ready(false);CHECK(box.request().argb.empty());box.publish(epoch,2,1,{1,2});CHECK(box.captures()==1);
 box.ready(true);CHECK(box.request().argb.empty());box.publish(epoch,2,1,{1,2});CHECK(box.captures()==1); // obsolete in-flight capture rejected
 box.supported(false);CHECK(box.request().argb.empty());CHECK(!box.takeRequest());
 vkr::PreviewMailbox changed;changed.supported(true);changed.ready(true);changed.request();
 changed.supported(false);CHECK(!changed.takeRequest()); // revoke queued capture immediately
 changed.supported(true);CHECK(!changed.takeRequest());changed.request();auto old=changed.takeRequest();CHECK(old);
 changed.supported(false);changed.supported(true);changed.publish(old,1,1,{1});
 CHECK(changed.captures()==0 && changed.request().argb.empty()); // old format result rejected
 return 0;
}
