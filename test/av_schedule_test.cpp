#include "av_schedule.h"
#include <cstdlib>
#include <cstdio>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(){
 // Storage headroom does not silently expand the timed queue or shift A/V PTS.
 CHECK(vkr::AvRingBudget::pendingSlots==6);
 // Thirteen: the IPC mailbox holds two unread frames (6a, 2026-11-27).
 CHECK(vkr::AvRingBudget::mailboxSlots==2 && vkr::AvRingBudget::ringSlots==13);
 CHECK(vkr::AvRingBudget::testRingSlots(nullptr)==13);
 CHECK(vkr::AvRingBudget::testRingSlots("13")==13);
 CHECK(vkr::AvRingBudget::testRingSlots("8")==8);
 CHECK(vkr::AvRingBudget::testRingSlots("12")==12);
 bool rejected=false;try{vkr::AvRingBudget::testRingSlots("12junk");}
 catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
 CHECK(vkr::AvRingBudget::maxLatencyNs(60,1)==100000000);
 CHECK(vkr::AvRingBudget::maxLatencyNs(30,1)==200000000);
 CHECK(vkr::AvRingBudget::maxLatencyNs(60000,1001)==100100000);
 CHECK(vkr::AvRingBudget::maxLatencyNs(0,1)==0);
 CHECK(vkr::AvRingBudget::maxLatencyNs(1,UINT32_MAX)==UINT64_MAX);
 vkr::AvSchedule s;using D=vkr::AvSchedule::Decision;
 // Observed ownership trace: ready112..119 are unavailable until consumed112.
 // The old ring has no writer although its six-future-frame queue is valid.
 // Additional storage fixes that opportunity without declaring old reads free.
 const uint64_t occupied[]={112,113,114,115,116,117,118,119};
 unsigned unavailable=0;for(auto ready:occupied)if(ready>111)++unavailable;
 CHECK(unavailable==vkr::AvRingBudget::testRingSlots("8"));
 CHECK(unavailable+1<=vkr::AvRingBudget::testRingSlots(nullptr));
 s.reset(3);CHECK(s.generation==3);
 CHECK(s.decide(20,100,100,10)==D::Hold);
 CHECK(s.decide(20,130,100,10)==D::Present);
 CHECK(s.decide(20,125,100,10,5)==D::Present);
 CHECK(s.decide(20,200,100,10,0,60)==D::Drop);
 CHECK(s.decide(-1,200,100,10)==D::Invalid);
 CHECK(s.decide(1,0,UINT64_MAX,1)==D::Invalid);
 CHECK(s.decide(1,UINT64_MAX,0,0,1)==D::Invalid);
 s.reset(4);CHECK(s.generation==4 && s.resets==2 && s.lastErrorNs==0);
 // Frame decisions use clock time, never source/receiver arrival time.
 for(int i=0;i<1000;++i)CHECK(s.decide(i*10000000ll,1000000000ull+i*10000000ull,900000000,100000000)==D::Present);
 CHECK(s.presented==1002 && s.late==1 && s.invalid==3);
 // Saturated steady-state queue: every due iteration must replenish one slot.
 // Old acquire-only-before-pop ordering misses the first full-queue opportunity.
 unsigned depth=6, acquired=0, oldDepth=6, oldAcquired=0;
 for(unsigned i=0;i<120;++i){
   vkr::AvAcquireBudget budget;
   unsigned available=1;
   auto acquire=[&]{if(!available)return false;--available;++depth;++acquired;return true;};
   CHECK(budget.poll(depth,6,acquire)==0);
   CHECK(depth>0);--depth;
   CHECK(budget.poll(depth,6,acquire)==1);
   CHECK(depth==6);
   if(oldDepth<6){++oldDepth;++oldAcquired;}
   --oldDepth;
 }
 CHECK(acquired==120 && oldAcquired<120);
 // A vsync-paced iteration that finds several announced frames takes them all,
 // up to the pending cap; one attempt per iteration let the IPC mailbox
 // replace the older one (6a, Linux FIFO at 60 Hz).
 {vkr::AvAcquireBudget b;unsigned d=2,avail=3,calls=0;
  CHECK(b.poll(d,6,[&]{++calls;if(!avail)return false;--avail;++d;return true;})==3 && d==5 && calls==4);}
 {vkr::AvAcquireBudget b;unsigned d=2,avail=9,calls=0;
  CHECK(b.poll(d,6,[&]{++calls;if(!avail)return false;--avail;++d;return true;})==4 && d==6 && calls==4 && avail==5);}
 // An empty channel costs one nonblocking attempt; a full queue costs none.
 {vkr::AvAcquireBudget b;unsigned calls=0;CHECK(b.poll(5,6,[&]{++calls;return false;})==0 && calls==1);}
 {vkr::AvAcquireBudget b;unsigned calls=0;CHECK(b.poll(6,6,[&]{++calls;return true;})==0 && calls==0);}
 return 0;
}
