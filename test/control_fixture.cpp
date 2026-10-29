#include "control_api.h"
#include "moderation.h"
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdio>
int main(int argc,char**argv){
 vkr::ModerationPolicy<4> m;bool live[4]={true,true,true,true};
 for(unsigned i=0;i<4;i++)m.ensureOrdered(i);
 m.maxVisible=2;m.rotateSeconds=1;
 if(m.select(live,0)!=std::vector<uint32_t>({0,1}) || m.select(live,1000000000)!=std::vector<uint32_t>({2,3}))return 2;
 m.pinned[3]=true;m.hidden[1]=true;m.spotlight=1;
 if(m.select(live,0)!=std::vector<uint32_t>({3,0}))return 3;
 m.spotlight=2;if(m.select(live,0)!=std::vector<uint32_t>({2}))return 4;
 live[2]=false;if(m.select(live,0)!=std::vector<uint32_t>({3,0}))return 5;
 if(argc<2)return 0;
 vkr::ControlApi api;vkr::ControlSnapshot s;s.layout="wall";s.cameraAvailable=false;s.previewAvailable=false;
 vkr::ControlStreamInfo stream;stream.slot=0;stream.connected=true;s.streams.push_back(stream);api.publish(s);
 if(!api.start(atoi(argv[1])))return 6;
 for(unsigned i=0;i<1000;i++){
  for(auto c:api.drainCommands()){
   if(c.type==vkr::ControlCommand::Type::Layout)s.layout=c.str;
   if(c.type==vkr::ControlCommand::Type::Pause)s.paused=c.i;
   if(c.type==vkr::ControlCommand::Type::Config){if(c.i>0)s.maxVisible=c.i;if(c.d>=1)s.rotateSeconds=c.d;}
  }
  api.publish(s);std::this_thread::sleep_for(std::chrono::milliseconds(10));
 }
 api.stop();return 0;
}
