#include "portable_output.h"
#include "perf_trace.h"
#include "log.h"
#include <algorithm>
#include <chrono>
#include <cstring>
namespace vkr {
namespace {
int64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
uint8_t clamp(int v){return (uint8_t)std::clamp(v,0,255);}
}
void PortableOutput::configure(const std::vector<std::string>& names) {
    for(const auto& name:names)if(!name.empty())outputs_.push_back({name});
    if(enabled())VKR_LOG("scene output: portable async readback/CPU conversion fallback enabled");
}
void PortableOutput::freeFrame(VkContext& vk, Readback& f) {
    if(f.data)vkUnmapMemory(vk.device,f.memory);
    if(f.buffer)vkDestroyBuffer(vk.device,f.buffer,nullptr);
    if(f.memory)vkFreeMemory(vk.device,f.memory,nullptr);
    f={};
}
void PortableOutput::retire(VkContext& vk,unsigned slot) {
    auto& f=frames_[slot%2]; if(!f.pending)return;
    f.pending=false;
    vkrtrace::Scope trace("service.output_publish");
    for(auto& output:outputs_) {
        const auto t=now();
        if(!output.channel && t>=output.retry) {
            output.retry=t+1000000000ll;
            flOpenProducer(output.name.c_str(),&output.channel);
        }
        if(!output.channel)continue;
        flChannelInfo info{};
        if(flQuery(output.channel,&info)!=FL_OK)continue;
        flBuffer buffer{};
        flResult r=flAcquireBuffer(output.channel,&buffer,0);
        if(r==FL_DISCONNECTED){flClose(output.channel);output.channel=nullptr;continue;}
        if(r!=FL_OK)continue;
        flMappedImage mapped{};
        r=flImageMapPlanes(buffer.image,&mapped);
        if(r!=FL_OK){VKR_WARN("output '%s': CPU mapping unavailable: %s",output.name.c_str(),flResultString(r));flClose(output.channel);output.channel=nullptr;continue;}
        auto rgb=[&](unsigned x,unsigned y,int& red,int& green,int& blue) {
            unsigned sx=(uint64_t)x*f.extent.width/info.width;
            unsigned sy=(uint64_t)y*f.extent.height/info.height;
            const auto* p=(const uint8_t*)f.data+((size_t)sy*f.extent.width+sx)*4;
            bool bgra=f.format==VK_FORMAT_B8G8R8A8_UNORM || f.format==VK_FORMAT_B8G8R8A8_SRGB;
            red=p[bgra?2:0];green=p[1];blue=p[bgra?0:2];
        };
        bool valid=(info.format==FL_FORMAT_NV12 && mapped.planes==2) ||
            ((info.format==FL_FORMAT_RGBA8 || info.format==FL_FORMAT_BGRA8 || info.format==FL_FORMAT_RGB10A2) && mapped.planes==1);
        if(valid)for(unsigned y=0;y<info.height;y++)for(unsigned x=0;x<info.width;x++) {
            int red,green,blue;rgb(x,y,red,green,blue);
            auto* row=(uint8_t*)mapped.plane[0].pixels+y*mapped.plane[0].stride;
            if(info.format==FL_FORMAT_NV12) {
                // BT.709 limited-range Y, 2x2 box-filtered chroma.
                row[x]=clamp(16+((47*red+157*green+16*blue+128)>>8));
                if(!(x&1) && !(y&1)) {
                    int rsum=0,gsum=0,bsum=0;
                    for(unsigned dy=0;dy<2;dy++)for(unsigned dx=0;dx<2;dx++) {
                        int r,g,b;rgb(x+dx,y+dy,r,g,b);rsum+=r;gsum+=g;bsum+=b;
                    }
                    auto* uv=(uint8_t*)mapped.plane[1].pixels+(y/2)*mapped.plane[1].stride+x;
                    uv[0]=clamp(128+((-26*rsum-87*gsum+112*bsum+512)>>10));
                    uv[1]=clamp(128+((112*rsum-102*gsum-10*bsum+512)>>10));
                }
            } else if(info.format==FL_FORMAT_RGB10A2) {
                uint32_t packed=(red*1023/255)|((green*1023/255)<<10)|((blue*1023/255)<<20)|(3u<<30);
                std::memcpy(row+x*4,&packed,4);
            } else {
                row[x*4]=(uint8_t)(info.format==FL_FORMAT_BGRA8?blue:red);
                row[x*4+1]=(uint8_t)green;row[x*4+2]=(uint8_t)(info.format==FL_FORMAT_BGRA8?red:blue);row[x*4+3]=255;
            }
        }
        flImageUnmap(buffer.image);
        if(valid)r=flSubmit(output.channel,&buffer,f.pts);
        if(!valid || r!=FL_OK){flClose(output.channel);output.channel=nullptr;}
    }
}
void PortableOutput::record(VkContext& vk,unsigned slot,VkCommandBuffer cmd,VkImage image,
        VkExtent2D extent,VkFormat format,VkImageLayout layout,int64_t pts) {
    if(!enabled())return;
    if(format!=VK_FORMAT_B8G8R8A8_UNORM && format!=VK_FORMAT_B8G8R8A8_SRGB &&
       format!=VK_FORMAT_R8G8B8A8_UNORM && format!=VK_FORMAT_R8G8B8A8_SRGB)return;
    auto& f=frames_[slot%2];
    if(f.extent.width!=extent.width || f.extent.height!=extent.height)freeFrame(vk,f);
    if(!f.buffer) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=(VkDeviceSize)extent.width*extent.height*4;
        bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if(vkCreateBuffer(vk.device,&bi,nullptr,&f.buffer)!=VK_SUCCESS)return;
        VkMemoryRequirements req{};vkGetBufferMemoryRequirements(vk.device,f.buffer,&req);
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(vk.phys,&props);
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<props.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i)) &&
            (props.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){type=i;break;}
        if(type==UINT32_MAX){freeFrame(vk,f);return;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        if(vkAllocateMemory(vk.device,&ai,nullptr,&f.memory)!=VK_SUCCESS ||
           vkBindBufferMemory(vk.device,f.buffer,f.memory,0)!=VK_SUCCESS ||
           vkMapMemory(vk.device,f.memory,0,VK_WHOLE_SIZE,0,&f.data)!=VK_SUCCESS){freeFrame(vk,f);return;}
        f.extent=extent;
    }
    auto barrier=[&](VkImageLayout from,VkImageLayout to) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask=b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask=b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout=from;b.newLayout=to;b.image=image;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.imageMemoryBarrierCount=1;d.pImageMemoryBarriers=&b;
        vkCmdPipelineBarrier2(cmd,&d);
    };
    barrier(layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};
    vkCmdCopyImageToBuffer(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.buffer,1,&copy);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,layout);
    f.format=format;f.pts=pts;f.armed=true;
}
void PortableOutput::destroy(VkContext& vk) {
    for(unsigned i=0;i<2;i++){retire(vk,i);freeFrame(vk,frames_[i]);}
    for(auto& o:outputs_)if(o.channel)flClose(o.channel);
    outputs_.clear();
}
}
