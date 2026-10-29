#include "portable_input.h"
#include "perf_trace.h"
#include <algorithm>
#include <cstring>
#include <vector>
namespace vkr {
bool uploadFrame(VkContext& vk,ExternalVideoPool& pool,VideoPass& pass,bool& bound,
                 flImage* image,uint32_t key,const flChannelInfo& info) {
    vkrtrace::Scope trace("service.input_cpu_conversion");
    flMappedImage m{};if(flImageMapPlanes(image,&m)!=FL_OK)return false;
    std::vector<uint8_t> bytes((size_t)info.width*info.height*4);
    bool valid=m.planes==(info.format==FL_FORMAT_NV12?2u:1u);
    for(unsigned y=0;valid && y<info.height;y++)for(unsigned x=0;x<info.width;x++) {
        auto* dst=bytes.data()+((size_t)y*info.width+x)*4;
        const auto* row=(const uint8_t*)m.plane[0].pixels+y*m.plane[0].stride;
        if(info.format==FL_FORMAT_NV12) {
            const auto* uv=(const uint8_t*)m.plane[1].pixels+(y/2)*m.plane[1].stride+(x&~1u);
            int yy=(int)row[x]-16,u=(int)uv[0]-128,v=(int)uv[1]-128;
            dst[2]=(uint8_t)std::clamp((298*yy+459*v+128)>>8,0,255);
            dst[1]=(uint8_t)std::clamp((298*yy-55*u-136*v+128)>>8,0,255);
            dst[0]=(uint8_t)std::clamp((298*yy+541*u+128)>>8,0,255);dst[3]=255;
        }else if(info.format==FL_FORMAT_RGB10A2) {
            uint32_t word;std::memcpy(&word,row+x*4,4);
            dst[2]=(word&1023)*255/1023;dst[1]=((word>>10)&1023)*255/1023;
            dst[0]=((word>>20)&1023)*255/1023;dst[3]=(word>>30)*85;
        }else if(info.format==FL_FORMAT_RGBA8 || info.format==FL_FORMAT_BGRA8) {
            const auto* src=row+x*4;bool rgba=info.format==FL_FORMAT_RGBA8;
            dst[0]=src[rgba?2:0];dst[1]=src[1];dst[2]=src[rgba?0:2];dst[3]=src[3];
        }else valid=false;
    }
    flImageUnmap(image);if(!valid)return false;
    bool added=!pool.find(key);
    if(added && !pool.addOwnedSurface(vk,key,info.width,info.height))return false;
    if(!pool.updateOwnedSurface(vk,key,bytes.data(),info.width*4,info.width,info.height))return false;
    if(!bound){bound=pass.bindPool(vk,pool);return bound;}
    return !added || pass.addSlot(vk,pool,key);
}
}
