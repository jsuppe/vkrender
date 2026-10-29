#pragma once
#include "vk_context.h"
#include "perf_trace.h"
#include <vector>

// Shared Android/Linux/Windows GPU-duration recorder. Call collect only once
// the existing frame fence is retired; never introduces a profiling wait.
struct FrameGpuTrace {
    VkQueryPool queries=VK_NULL_HANDLE;
    bool attempted=false,pending=false;
    uint32_t validBits=0;
    double period=0;
    uint64_t frame=0;
    void collect(vkr::VkContext& vk) {
        if(!pending)return;
        uint64_t ticks[2]{};
        VkResult r=vkGetQueryPoolResults(vk.device,queries,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
        if(r==VK_SUCCESS) {
            uint64_t delta=ticks[1]-ticks[0];
            if(validBits<64)delta&=(1ull<<validBits)-1;
            vkrtrace::record({"gpu.scene_duration",vkrtrace::clockNs(),(uint64_t)(delta*period),0,frame,0,0,0,0});
        }else vkrtrace::record({"gpu.scene_query_unavailable",vkrtrace::clockNs(),0,0,frame,0,0,0,r});
        pending=false;
    }
    void prepare(vkr::VkContext& vk) {
        if(!vkrtrace::gpuEnabled())return;
        if(!attempted) {
            attempted=true;
            uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(vk.phys,&count,nullptr);
            std::vector<VkQueueFamilyProperties> props(count);
            vkGetPhysicalDeviceQueueFamilyProperties(vk.phys,&count,props.data());
            VkPhysicalDeviceProperties device{};vkGetPhysicalDeviceProperties(vk.phys,&device);
            validBits=props[vk.queueFamily].timestampValidBits;period=device.limits.timestampPeriod;
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;
            if(!validBits || vkCreateQueryPool(vk.device,&qi,nullptr,&queries)!=VK_SUCCESS) {
                queries=VK_NULL_HANDLE;
                vkrtrace::record({"gpu.scene_unavailable",vkrtrace::clockNs(),0,0,0,0,0,0,-1});
            }
        }
        collect(vk);
    }
    void begin(VkCommandBuffer cmd) {
        if(!queries)return;
        vkCmdResetQueryPool(cmd,queries,0,2);
        vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
    }
    void end(VkCommandBuffer cmd) {
        if(queries)vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,1);
    }
    void submitted(uint64_t id){if(queries){pending=true;frame=id;}}
    void destroy(vkr::VkContext& vk){collect(vk);if(queries)vkDestroyQueryPool(vk.device,queries,nullptr);queries=VK_NULL_HANDLE;}
};
