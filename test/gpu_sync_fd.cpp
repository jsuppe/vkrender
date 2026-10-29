#include "vk_context.h"
#include "framelink.h"
#include "framelink_sync_fd.h"
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
int main(){
 alarm(15); setbuf(stdout,nullptr); puts("initializing Vulkan");
 vkr::VkContext vk;CHECK(vk.init(64,64,"GPU completion test",true));
 if(!vk.hasSyncFd){puts("SKIP: external semaphore FD extension unavailable");vk.destroy();return 77;}
 VkPhysicalDeviceExternalSemaphoreInfo query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
 query.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
 VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
 vkGetPhysicalDeviceExternalSemaphoreProperties(vk.phys,&query,&properties);
 if(!(properties.externalSemaphoreFeatures&VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)){
  puts("SKIP: SYNC_FD export unavailable");vk.destroy();return 77;
 }
 VkCommandPool pool{}; VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pci.queueFamilyIndex=vk.queueFamily;
 CHECK(vkCreateCommandPool(vk.device,&pci,nullptr,&pool)==VK_SUCCESS);
 VkEvent events[2]{}; VkCommandBuffer commands[2]{};
 VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=2;
 CHECK(vkAllocateCommandBuffers(vk.device,&ai,commands)==VK_SUCCESS);
 for(int i=0;i<2;i++) CHECK(vkCreateEvent(vk.device,&ei,nullptr,&events[i])==VK_SUCCESS);
 auto queuedFence=[&](uint64_t value,VkSemaphore& binary){
  VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&external;
  CHECK(vkCreateSemaphore(vk.device,&si,nullptr,&binary)==VK_SUCCESS);
  auto cmd=commands[value-1]; VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
  vkCmdWaitEvents(cmd,1,&events[value-1],VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,nullptr,0,nullptr,0,nullptr);
  CHECK(vkEndCommandBuffer(cmd)==VK_SUCCESS);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&binary;
  puts("queue event-gated signal"); CHECK(vkQueueSubmit(vk.queue,1,&submit,VK_NULL_HANDLE)==VK_SUCCESS); puts("export SYNC_FD");
  VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};get.semaphore=binary;get.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  int fd=-1;CHECK(vkGetSemaphoreFdKHR(vk.device,&get,&fd)==VK_SUCCESS);CHECK(fd>=0);puts("export complete");return fd;
 };
 auto signal=[&](uint64_t value){CHECK(vkSetEvent(vk.device,events[value-1])==VK_SUCCESS);};
 flChannel *c=nullptr,*p=nullptr;CHECK(flCreateChannelEx("gpu-sync-fd",64,64,FL_FORMAT_RGBA8,1,FL_MAP_CPU,&c)==FL_OK);
 for(int i=0;i<100&&!p;i++){flOpenProducer("gpu-sync-fd",&p);std::this_thread::sleep_for(std::chrono::milliseconds(5));}CHECK(p);
 puts("channel connected"); flBuffer b{};CHECK(flAcquireBuffer(p,&b,100)==FL_OK);
 VkSemaphore ready,consumed;int fd=queuedFence(1,ready);CHECK(flSubmitSyncFd(p,&b,fd,123)==FL_OK);close(fd);
 flFrame f{};CHECK(flAcquireFrame(c,&f,20)==FL_TIMEOUT);signal(1);CHECK(flAcquireFrame(c,&f,2000)==FL_OK);
 fd=queuedFence(2,consumed);CHECK(flReleaseSyncFd(c,&f,fd)==FL_OK);close(fd);
 CHECK(flAcquireBuffer(p,&b,20)==FL_TIMEOUT);signal(2);CHECK(flAcquireBuffer(p,&b,2000)==FL_OK);
 flClose(p);flClose(c);vkDeviceWaitIdle(vk.device);
 vkDestroySemaphore(vk.device,ready,nullptr);vkDestroySemaphore(vk.device,consumed,nullptr);for(auto event:events)vkDestroyEvent(vk.device,event,nullptr);vkDestroyCommandPool(vk.device,pool,nullptr);vk.destroy();
 puts("PASS: real Vulkan SYNC_FD ready/consumed handoff and timeout/reuse ordering");
}
