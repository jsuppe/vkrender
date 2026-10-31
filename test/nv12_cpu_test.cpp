#include "nv12_cpu.h"
#include <vector>
#include <random>
#include <cstdio>
int main(){
 std::mt19937 rng(497);
 for(unsigned w:{2u,4u,6u,14u,1920u})for(unsigned h:{2u,10u}) {
  size_t ys=w+7,us=w+11,ds=w*4+13;
  std::vector<uint8_t> y(ys*h),uv(us*(h/2)),out(ds*h,0xA5);
  for(auto&v:y)v=uint8_t(rng());for(auto&v:uv)v=uint8_t(rng());
  vkr::nv12ToBgra(y.data(),ys,uv.data(),us,out.data(),ds,w,h);
  for(unsigned j=0;j<h;++j){for(unsigned i=0;i<w;++i){
   int yy=int(y[j*ys+i])-16,u=int(uv[(j/2)*us+(i&~1u)])-128,v=int(uv[(j/2)*us+(i&~1u)+1])-128;
   int expected[]={std::clamp((298*yy+541*u+128)>>8,0,255),std::clamp((298*yy-55*u-136*v+128)>>8,0,255),std::clamp((298*yy+459*v+128)>>8,0,255),255};
   for(unsigned k=0;k<4;++k)if(out[j*ds+i*4+k]!=expected[k]){std::fprintf(stderr,"mismatch %u,%u component%u\n",i,j,k);return 1;}
  }for(size_t i=w*4;i<ds;++i)if(out[j*ds+i]!=0xA5)return 2;}
 }
 return 0;
}
