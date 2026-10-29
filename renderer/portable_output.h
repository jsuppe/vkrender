#pragma once
#include "vk_context.h"
#include "framelink.h"
#include <string>
#include <vector>
#ifndef _WIN32
#include "gpu_output.h"
#endif
namespace vkr {
// Portable fallback: asynchronous Vulkan readback, then CPU conversion/upload.
// Call retire only after the caller's existing frame fence has completed.
// No extra GPU wait is introduced; this is not a zero-copy output path.
class PortableOutput {
public:
    void configure(const std::vector<std::string>& names);
    bool enabled() const { return !outputs_.empty(); }
    void retire(VkContext&, unsigned slot);
    void record(VkContext&, unsigned slot, VkCommandBuffer, VkImage, VkExtent2D,
                VkFormat, VkImageLayout, int64_t pts);
    void submitted(unsigned slot) {
#ifndef _WIN32
        if(gpu_){gpu_->submitted(slot);return;}
#endif
        auto& f=frames_[slot%2]; f.pending=f.armed; f.armed=false; }
    void destroy(VkContext&);
private:
#ifndef _WIN32
    std::unique_ptr<GpuOutput> gpu_;
#endif
    struct Readback {
        VkBuffer buffer=VK_NULL_HANDLE;
        VkDeviceMemory memory=VK_NULL_HANDLE;
        void* data=nullptr;
        VkExtent2D extent{};
        VkFormat format=VK_FORMAT_UNDEFINED;
        int64_t pts=0;
        bool pending=false,armed=false;
    } frames_[2];
    struct Output {std::string name; flChannel* channel=nullptr; int64_t retry=0;};
    std::vector<Output> outputs_;
    void freeFrame(VkContext&, Readback&);
};
}
