#pragma once
#ifdef __ANDROID__
#include "vk_context.h"
#include <memory>
#include <string>
#include <vector>
struct ANativeWindow;
// Takes ownership of one acquired window reference; null means detach.
void vkrSetDisplayWindow(int id, ANativeWindow* window, float x,float y,float width,float height,bool recreate=false);
std::string vkrDisplayStatus();
namespace vkr {
class AndroidDisplays {
public:
    AndroidDisplays();
    ~AndroidDisplays();
    // All Vulkan operations are confined to the renderer thread.
    void update(VkContext& vk);
    void record(VkContext& vk, VkCommandBuffer cmd, unsigned frame,
                VkImage source, VkExtent2D extent,
                std::vector<VkSemaphore>& waits,std::vector<VkPipelineStageFlags>& stages,
                std::vector<VkSemaphore>& signals);
    void present(VkContext& vk);
    void destroy(VkContext& vk);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
#endif
