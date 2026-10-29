#pragma once
#include "vk_context.h"
#include "preview_mailbox.h"
namespace vkr {
class AndroidPreview {
 public:
    explicit AndroidPreview(PreviewMailbox& mailbox):mailbox_(mailbox){}
    bool supported(VkContext& vk);
    void record(VkContext& vk,unsigned slot,VkCommandBuffer cmd,VkImage image,VkExtent2D extent,VkFormat format,VkImageLayout layout);
    void submitted(unsigned slot);
    void retire(unsigned slot);
    void destroy(VkContext& vk);
 private:
    struct Frame {VkImage source{};VkDeviceMemory sourceMemory{};VkExtent2D sourceExtent{};VkImage image{};VkDeviceMemory imageMemory{};VkBuffer buffer{};VkDeviceMemory memory{};void* mapped{};
        VkExtent2D extent{};VkFormat format{};uint64_t epoch=0;bool armed=false,pending=false;};
    Frame frames_[2];PreviewMailbox& mailbox_;
    static void clear(VkContext& vk,Frame& frame);
    static bool allocate(VkContext& vk,Frame& frame,VkExtent2D extent,VkExtent2D sourceExtent,VkFormat format);
};
}
