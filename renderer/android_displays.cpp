#include "android_displays.h"
#ifdef __ANDROID__
#include <android/native_window.h>
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <cmath>
#include <sstream>
#include "log.h"
namespace {
struct WindowRef {
    ANativeWindow* window;
    explicit WindowRef(ANativeWindow* w):window(w){}
    ~WindowRef(){if(window)ANativeWindow_release(window);}
};
struct Request {
    std::shared_ptr<WindowRef> window;
    std::array<float,4> crop={0,0,1,1};
    bool recreate=false;
};
std::mutex requestsMutex;
std::map<int,Request> requests;
std::string status="[]";
}
std::string vkrDisplayStatus(){std::lock_guard<std::mutex> lock(requestsMutex);return status;}
void vkrSetDisplayWindow(int id,ANativeWindow* window,float x,float y,float w,float h,bool recreate) {
    Request request{std::make_shared<WindowRef>(window),{x,y,w,h},recreate};
    if(id<0||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(w)||!std::isfinite(h)
       ||x<0||y<0||w<=0||h<=0||x+w>1.00001f||y+h>1.00001f) {
        VKR_WARN("display: invalid attach/crop for display %d",id);return;
    }
    std::lock_guard<std::mutex> lock(requestsMutex);
    auto previous=requests.find(id);
    if(previous!=requests.end()&&previous->second.window&&previous->second.window->window==window)
        request.recreate=request.recreate||previous->second.recreate;
    requests[id]=std::move(request); // superseded requests release their window ref
}
namespace vkr {
struct AndroidDisplays::Impl {
    struct Output {
        Request request;
        VkSurfaceKHR surface=VK_NULL_HANDLE;
        VkSwapchainKHR swapchain=VK_NULL_HANDLE;
        VkExtent2D extent{};
        std::vector<VkImage> images;
        std::vector<VkSemaphore> done; // indexed by acquired image, not frame
        VkSemaphore acquire[2]={};
        uint32_t image=0;
        bool acquired=false,dirty=false;
        uint64_t frames=0,unavailable=0;
    };
    struct Stage {VkImage image=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkExtent2D extent{};};
    Stage stage[2];
    std::map<int,Output> outputs;
    void publishStatus() {
        std::ostringstream json;json<<"[";bool first=true;
        for(auto& [id,o]:outputs) {
            if(!first)json<<",";first=false;
            json<<"{\"id\":"<<id<<",\"width\":"<<o.extent.width<<",\"height\":"<<o.extent.height
                <<",\"presented\":"<<o.frames<<",\"unavailable\":"<<o.unavailable<<"}";
        }
        json<<"]";std::lock_guard<std::mutex> lock(requestsMutex);status=json.str();
    }
    void clearStage(VkContext& vk,Stage& s) {
        if(s.image)vkDestroyImage(vk.device,s.image,nullptr);
        if(s.memory)vkFreeMemory(vk.device,s.memory,nullptr);
        s={};
    }
    bool ensureStage(VkContext& vk,Stage& s,VkExtent2D extent) {
        if(s.image&&s.extent.width==extent.width&&s.extent.height==extent.height)return true;
        clearStage(vk,s);
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;
        ci.format=vk.swapFormat;ci.extent={extent.width,extent.height,1};ci.mipLevels=1;ci.arrayLayers=1;
        ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
        ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        if(vkCreateImage(vk.device,&ci,nullptr,&s.image)!=VK_SUCCESS)return false;
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(vk.device,s.image,&req);
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(vk.phys,&props);
        uint32_t type=UINT32_MAX;
        for(uint32_t i=0;i<props.memoryTypeCount;i++)if((req.memoryTypeBits&(1u<<i))&&
                (props.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=i;break;}
        if(type==UINT32_MAX){clearStage(vk,s);return false;}
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
        if(vkAllocateMemory(vk.device,&ai,nullptr,&s.memory)!=VK_SUCCESS||vkBindImageMemory(vk.device,s.image,s.memory,0)!=VK_SUCCESS){clearStage(vk,s);return false;}
        s.extent=extent;return true;
    }
    void clear(VkContext& vk,Output& o) {
        for(auto sem:o.done)if(sem)vkDestroySemaphore(vk.device,sem,nullptr);
        for(auto sem:o.acquire)if(sem)vkDestroySemaphore(vk.device,sem,nullptr);
        if(o.swapchain)vkDestroySwapchainKHR(vk.device,o.swapchain,nullptr);
        if(o.surface)vkDestroySurfaceKHR(vk.instance,o.surface,nullptr);
        o.done.clear();o.images.clear();o.acquire[0]=o.acquire[1]=VK_NULL_HANDLE;
        o.swapchain=VK_NULL_HANDLE;o.surface=VK_NULL_HANDLE;o.acquired=false;
    }
    bool create(VkContext& vk,int id,Output& o) {
        VkAndroidSurfaceCreateInfoKHR ai{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        ai.window=o.request.window->window;
        if(vkCreateAndroidSurfaceKHR(vk.instance,&ai,nullptr,&o.surface)!=VK_SUCCESS)return false;
        VkBool32 supported=VK_FALSE;
        if(vkGetPhysicalDeviceSurfaceSupportKHR(vk.phys,vk.queueFamily,o.surface,&supported)!=VK_SUCCESS||!supported)return false;
        VkSurfaceCapabilitiesKHR caps{};
        if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.phys,o.surface,&caps)!=VK_SUCCESS)return false;
        const VkImageUsageFlags requiredUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if((caps.supportedUsageFlags&requiredUsage)!=requiredUsage)return false;
        uint32_t count=0;
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.phys,o.surface,&count,nullptr)!=VK_SUCCESS||!count)return false;
        std::vector<VkSurfaceFormatKHR> formats(count);
        if(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.phys,o.surface,&count,formats.data())!=VK_SUCCESS)return false;
        auto chosen=formats.front(); bool found=false;
        for(auto f:formats) {
            // Stay in the same color space; this is an image copy, not HDR conversion.
            if(f.colorSpace!=VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)continue;
            VkFormatProperties props{};vkGetPhysicalDeviceFormatProperties(vk.phys,f.format,&props);
            if(props.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT){chosen=f;found=true;if(f.format==vk.swapFormat)break;}
        }
        VkFormatProperties src{};vkGetPhysicalDeviceFormatProperties(vk.phys,vk.swapFormat,&src);
        if(!found||!(src.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT))return false;
        o.extent=caps.currentExtent;
        if(o.extent.width==UINT32_MAX) {
            o.extent.width=std::clamp((uint32_t)ANativeWindow_getWidth(ai.window),caps.minImageExtent.width,caps.maxImageExtent.width);
            o.extent.height=std::clamp((uint32_t)ANativeWindow_getHeight(ai.window),caps.minImageExtent.height,caps.maxImageExtent.height);
        }
        if(!o.extent.width||!o.extent.height)return false;
        VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        si.surface=o.surface;si.minImageCount=caps.minImageCount+2;
        if(caps.maxImageCount)si.minImageCount=std::min(si.minImageCount,caps.maxImageCount);
        si.imageFormat=chosen.format;si.imageColorSpace=chosen.colorSpace;
        si.imageExtent=o.extent;si.imageArrayLayers=1;si.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        si.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;
        si.preTransform=(caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)?VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:caps.currentTransform;
        si.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if(!(caps.supportedCompositeAlpha&si.compositeAlpha)) {
            for(auto a:{VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
                if(caps.supportedCompositeAlpha&a){si.compositeAlpha=a;break;}
        }
        si.presentMode=VK_PRESENT_MODE_FIFO_KHR;si.clipped=VK_TRUE;
        if(vkCreateSwapchainKHR(vk.device,&si,nullptr,&o.swapchain)!=VK_SUCCESS)return false;
        if(vkGetSwapchainImagesKHR(vk.device,o.swapchain,&count,nullptr)!=VK_SUCCESS||!count)return false;
        o.images.resize(count);o.done.resize(count);
        if(vkGetSwapchainImagesKHR(vk.device,o.swapchain,&count,o.images.data())!=VK_SUCCESS)return false;
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for(auto& s:o.acquire)if(vkCreateSemaphore(vk.device,&sem,nullptr,&s)!=VK_SUCCESS)return false;
        for(auto& s:o.done)if(vkCreateSemaphore(vk.device,&sem,nullptr,&s)!=VK_SUCCESS)return false;
        VKR_LOG("display: attached id=%d %ux%u images=%u crop=%.3f,%.3f,%.3f,%.3f",id,o.extent.width,o.extent.height,count,
                o.request.crop[0],o.request.crop[1],o.request.crop[2],o.request.crop[3]);
        return true;
    }
};
AndroidDisplays::AndroidDisplays():impl(new Impl){}
AndroidDisplays::~AndroidDisplays()=default;
void AndroidDisplays::update(VkContext& vk) {
    std::map<int,Request> changes;
    {std::lock_guard<std::mutex> lock(requestsMutex);changes.swap(requests);}
    for(auto& [id,o]:impl->outputs)if(o.dirty&&!changes.count(id))changes[id]=o.request;
    for(auto it=changes.begin();it!=changes.end();) {
        auto output=impl->outputs.find(it->first);
        if(output!=impl->outputs.end()&&!output->second.dirty&&!it->second.recreate&&it->second.window&&
           it->second.window->window==output->second.request.window->window) {
            VkSurfaceCapabilitiesKHR caps{};
            if(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.phys,output->second.surface,&caps)==VK_SUCCESS&&
               caps.currentExtent.width==output->second.extent.width&&caps.currentExtent.height==output->second.extent.height) {
                output->second.request.crop=it->second.crop;
                VKR_LOG("display: crop updated id=%d %.3f,%.3f,%.3f,%.3f",it->first,it->second.crop[0],it->second.crop[1],it->second.crop[2],it->second.crop[3]);
                it=changes.erase(it);continue;
            }
        }
        ++it;
    }
    if(changes.empty())return;
    // Lifecycle-only quiescence includes presentation completion before destroying
    // present semaphores/swapchains. Never called on the steady-state frame path.
    vkQueueWaitIdle(vk.queue);
    for(auto& [id,request]:changes) {
        auto it=impl->outputs.find(id);
        if(it!=impl->outputs.end()) {
            VKR_LOG("display: detached id=%d presented=%llu unavailable=%llu",id,
                    (unsigned long long)it->second.frames,(unsigned long long)it->second.unavailable);
            impl->clear(vk,it->second);impl->outputs.erase(it);
        }
        if(!request.window||!request.window->window)continue;
        auto& output=impl->outputs[id];output.request=std::move(request);
        if(!impl->create(vk,id,output)) {
            VKR_WARN("display: attach failed id=%d (surface/copy capability)",id);
            impl->clear(vk,output);impl->outputs.erase(id);
        }
    }
    impl->publishStatus();
}
void AndroidDisplays::record(VkContext& vk,VkCommandBuffer cmd,unsigned frame,VkImage source,VkExtent2D size,
                             std::vector<VkSemaphore>& waits,std::vector<VkPipelineStageFlags>& stages,
                             std::vector<VkSemaphore>& signals) {
    bool sourceTransitioned=false;
    if(impl->outputs.empty())return;
    auto& stage=impl->stage[frame%2];
    if(!impl->ensureStage(vk,stage,size)){VKR_WARN("display: staging allocation failed");return;}
    auto barrier=[&](VkImage image,VkImageLayout oldLayout,VkImageLayout newLayout,VkPipelineStageFlags2 srcStage,
                     VkAccessFlags2 srcAccess,VkPipelineStageFlags2 dstStage,VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.image=image;b.oldLayout=oldLayout;b.newLayout=newLayout;b.srcStageMask=srcStage;b.srcAccessMask=srcAccess;
        b.dstStageMask=dstStage;b.dstAccessMask=dstAccess;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=1;dep.pImageMemoryBarriers=&b;
        vkCmdPipelineBarrier2(cmd,&dep);
    };
    for(auto& [id,o]:impl->outputs) {
        o.acquired=false;
        VkResult result=vkAcquireNextImageKHR(vk.device,o.swapchain,0,o.acquire[frame%2],VK_NULL_HANDLE,&o.image);
        if(result==VK_NOT_READY||result==VK_TIMEOUT){++o.unavailable;continue;}
        if(result!=VK_SUCCESS&&result!=VK_SUBOPTIMAL_KHR){o.dirty=true;continue;}
        if(result==VK_SUBOPTIMAL_KHR)o.dirty=true;
        o.acquired=true;
        if(!o.frames)VKR_LOG("display: first copy id=%d source=%ux%u destination=%ux%u",id,size.width,size.height,o.extent.width,o.extent.height);
        if(!sourceTransitioned) {
            barrier(source,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);sourceTransitioned=true;
            barrier(stage.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageCopy copy{};copy.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.dstSubresource=copy.srcSubresource;
            copy.extent={size.width,size.height,1};
            vkCmdCopyImage(cmd,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stage.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
            barrier(stage.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        barrier(o.images[o.image],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit blit{};
        blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstSubresource=blit.srcSubresource;
        auto crop=o.request.crop;
        int x=std::clamp((int)(crop[0]*size.width),0,(int)size.width-1);
        int y=std::clamp((int)(crop[1]*size.height),0,(int)size.height-1);
        blit.srcOffsets[0]={x,y,0};
        blit.srcOffsets[1]={std::clamp((int)((crop[0]+crop[2])*size.width),x+1,(int)size.width),
                            std::clamp((int)((crop[1]+crop[3])*size.height),y+1,(int)size.height),1};
        blit.dstOffsets[1]={(int)o.extent.width,(int)o.extent.height,1};
        vkCmdBlitImage(cmd,stage.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,o.images[o.image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
        barrier(o.images[o.image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_NONE,0);
        waits.push_back(o.acquire[frame%2]);stages.push_back(VK_PIPELINE_STAGE_TRANSFER_BIT);signals.push_back(o.done[o.image]);
    }
    if(sourceTransitioned)barrier(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}
void AndroidDisplays::present(VkContext& vk) {
    for(auto& [id,o]:impl->outputs)if(o.acquired) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&o.done[o.image];
        pi.swapchainCount=1;pi.pSwapchains=&o.swapchain;pi.pImageIndices=&o.image;
        VkResult result=vkQueuePresentKHR(vk.queue,&pi);
        if(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR)++o.frames;
        if(result!=VK_SUCCESS)o.dirty=true;
        o.acquired=false;
        if(o.frames==1||o.frames%60==0)impl->publishStatus();
    }
}
void AndroidDisplays::destroy(VkContext& vk) {
    vkQueueWaitIdle(vk.queue);
    for(auto& [id,o]:impl->outputs) {
        VKR_LOG("display: shutdown id=%d presented=%llu unavailable=%llu",id,(unsigned long long)o.frames,(unsigned long long)o.unavailable);
        impl->clear(vk,o);
    }
    impl->outputs.clear();
    for(auto& s:impl->stage)impl->clearStage(vk,s);
    impl->publishStatus();
}
}
#endif
