#include "vk_context.h"
#include "framelink.h"
#include "framelink_sync_fd.h"
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/sync_file.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line%d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static int pollState(int fd,int milliseconds) {
 pollfd p{fd,POLLIN,0};int rc=poll(&p,1,milliseconds);
 CHECK(rc>=0);return p.revents;
}
static int unixDuplicate(int fd) {
 int pair[2];CHECK(socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair)==0);
 char byte='x';iovec iov{&byte,1};alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
 msghdr message{};message.msg_iov=&iov;message.msg_iovlen=1;
 message.msg_control=control;message.msg_controllen=sizeof(control);
 auto* c=CMSG_FIRSTHDR(&message);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));
 std::memcpy(CMSG_DATA(c),&fd,sizeof(fd));CHECK(sendmsg(pair[0],&message,0)==1);
 std::memset(control,0,sizeof(control));CHECK(recvmsg(pair[1],&message,0)==1);
 c=CMSG_FIRSTHDR(&message);CHECK(c&&c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS);
 int copy=-1;std::memcpy(&copy,CMSG_DATA(c),sizeof(copy));close(pair[0]);close(pair[1]);return copy;
}
static void describeFd(int fd,const char* stage) {
 char path[80],target[256]{};snprintf(path,sizeof(path),"/proc/self/fd/%d",fd);
 ssize_t count=readlink(path,target,sizeof(target)-1);if(count>=0)target[count]=0;
 sync_file_info info{};int rc=ioctl(fd,SYNC_IOC_FILE_INFO,&info);
 printf("fd stage=%s target=%s sync_info_rc=%d status=%d fences=%u\n",stage,target,rc,info.status,info.num_fences);
}
static int eventfdControl() {
 // Only a transport/readiness control, NOT evidence of GPU fence correctness.
 flChannel *c=nullptr,*p=nullptr;std::string name="eventfd-control-"+std::to_string(getpid());
 CHECK(flCreateChannelEx(name.c_str(),64,64,FL_FORMAT_RGBA8,1,FL_MAP_CPU,&c)==FL_OK);
 for(int i=0;i<100&&!p;i++){flOpenProducer(name.c_str(),&p);std::this_thread::sleep_for(std::chrono::milliseconds(5));}CHECK(p);
 flBuffer b{};CHECK(flAcquireBuffer(p,&b,100)==FL_OK);
 int ready=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK),consumed=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);CHECK(ready>=0&&consumed>=0);
 CHECK(flSubmitSyncFd(p,&b,ready,123)==FL_OK);flFrame frame{};
 auto before=flAcquireFrame(c,&frame,20);printf("eventfd ready-before=%d (%s)\n",before,flResultString(before));CHECK(before==FL_TIMEOUT);
 uint64_t one=1;CHECK(write(ready,&one,sizeof(one))==sizeof(one));CHECK(flAcquireFrame(c,&frame,2000)==FL_OK);
 CHECK(flReleaseSyncFd(c,&frame,consumed)==FL_OK);
 before=flAcquireBuffer(p,&b,20);printf("eventfd consumed-before=%d (%s)\n",before,flResultString(before));CHECK(before==FL_TIMEOUT);
 CHECK(write(consumed,&one,sizeof(one))==sizeof(one));CHECK(flAcquireBuffer(p,&b,2000)==FL_OK);
 CHECK(flCancelBuffer(p,&b)==FL_OK);flClose(p);flClose(c);close(ready);close(consumed);
 puts("PASS: eventfd transport control preserves readiness and consumed waits (not GPU proof)");return 0;
}
int main(int argc,char** argv){
 alarm(15); setbuf(stdout,nullptr); puts("initializing Vulkan");
 if(argc==2&&!std::strcmp(argv[1],"--eventfd-control"))return eventfdControl();
 const bool rawProbe=argc==2&&!std::strcmp(argv[1],"--raw-fence");
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
 VkEvent events[2]{}; VkCommandBuffer commands[2]{};VkFence completions[2]{};
 VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
 VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
 ai.commandPool=pool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=2;
 CHECK(vkAllocateCommandBuffers(vk.device,&ai,commands)==VK_SUCCESS);
 for(int i=0;i<2;i++) {CHECK(vkCreateEvent(vk.device,&ei,nullptr,&events[i])==VK_SUCCESS);
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};CHECK(vkCreateFence(vk.device,&fi,nullptr,&completions[i])==VK_SUCCESS);}
 auto queuedFence=[&](uint64_t value,VkSemaphore& binary){
  VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};si.pNext=&external;
  CHECK(vkCreateSemaphore(vk.device,&si,nullptr,&binary)==VK_SUCCESS);
  auto cmd=commands[value-1]; VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(vkBeginCommandBuffer(cmd,&begin)==VK_SUCCESS);
  vkCmdWaitEvents(cmd,1,&events[value-1],VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,nullptr,0,nullptr,0,nullptr);
  CHECK(vkEndCommandBuffer(cmd)==VK_SUCCESS);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&cmd;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&binary;
  puts("queue event-gated signal"); CHECK(vkQueueSubmit(vk.queue,1,&submit,completions[value-1])==VK_SUCCESS); puts("export SYNC_FD");
  VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};get.semaphore=binary;get.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  int fd=-1;CHECK(vkGetSemaphoreFdKHR(vk.device,&get,&fd)==VK_SUCCESS);CHECK(fd>=0);puts("export complete");return fd;
 };
 auto signal=[&](uint64_t value){CHECK(vkSetEvent(vk.device,events[value-1])==VK_SUCCESS);};
 if(rawProbe) {
  VkPhysicalDeviceProperties gpu{};vkGetPhysicalDeviceProperties(vk.phys,&gpu);
  printf("raw Vulkan device=%s api=%u driver=%u\n",gpu.deviceName,gpu.apiVersion,gpu.driverVersion);
  VkSemaphore semaphore;int fd=queuedFence(1,semaphore);describeFd(fd,"export-before-signal");
  int originalBefore=pollState(fd,20),copy=unixDuplicate(fd),copyBefore=pollState(copy,0);
  VkResult eventBefore=vkGetEventStatus(vk.device,events[0]),completionBefore=vkGetFenceStatus(vk.device,completions[0]);
  printf("raw-before original_revents=%d scm_rights_revents=%d event=%d completion=%d\n",originalBefore,copyBefore,eventBefore,completionBefore);
  signal(1);VkResult waited=vkWaitForFences(vk.device,1,&completions[0],VK_TRUE,2000000000ull);CHECK(waited==VK_SUCCESS);
  int originalAfter=pollState(fd,2000),copyAfter=pollState(copy,0);describeFd(fd,"export-after-signal");
  printf("{\"case\":\"raw-vulkan-fence\",\"before_pollin\":%s,\"scm_before_pollin\":%s,\"event_before\":%d,\"completion_before\":%d,\"after_pollin\":%s,\"scm_after_pollin\":%s,\"completion_after\":%d}\n",
    (originalBefore&POLLIN)?"true":"false",(copyBefore&POLLIN)?"true":"false",eventBefore,completionBefore,
    (originalAfter&POLLIN)?"true":"false",(copyAfter&POLLIN)?"true":"false",vkGetFenceStatus(vk.device,completions[0]));
  close(fd);close(copy);vkDestroySemaphore(vk.device,semaphore,nullptr);
  for(int i=0;i<2;i++){vkDestroyEvent(vk.device,events[i],nullptr);vkDestroyFence(vk.device,completions[i],nullptr);}
  vkDestroyCommandPool(vk.device,pool,nullptr);vk.destroy();return 0;
 }
 flChannel *c=nullptr,*p=nullptr;CHECK(flCreateChannelEx("gpu-sync-fd",64,64,FL_FORMAT_RGBA8,1,FL_MAP_CPU,&c)==FL_OK);
 for(int i=0;i<100&&!p;i++){flOpenProducer("gpu-sync-fd",&p);std::this_thread::sleep_for(std::chrono::milliseconds(5));}CHECK(p);
 puts("channel connected"); flBuffer b{};CHECK(flAcquireBuffer(p,&b,100)==FL_OK);
 VkSemaphore ready,consumed;int fd=queuedFence(1,ready);CHECK(flSubmitSyncFd(p,&b,fd,123)==FL_OK);close(fd);
 flFrame f{};const auto early=flAcquireFrame(c,&f,20);
 printf("ready-before-signal result=%d (%s)\n",early,flResultString(early));
 CHECK(early==FL_TIMEOUT);signal(1);CHECK(flAcquireFrame(c,&f,2000)==FL_OK);
 fd=queuedFence(2,consumed);CHECK(flReleaseSyncFd(c,&f,fd)==FL_OK);close(fd);
 CHECK(flAcquireBuffer(p,&b,20)==FL_TIMEOUT);signal(2);CHECK(flAcquireBuffer(p,&b,2000)==FL_OK);
 flClose(p);flClose(c);vkDeviceWaitIdle(vk.device);
 vkDestroySemaphore(vk.device,ready,nullptr);vkDestroySemaphore(vk.device,consumed,nullptr);for(int i=0;i<2;i++){vkDestroyEvent(vk.device,events[i],nullptr);vkDestroyFence(vk.device,completions[i],nullptr);}vkDestroyCommandPool(vk.device,pool,nullptr);vk.destroy();
 puts("PASS: real Vulkan SYNC_FD ready/consumed handoff and timeout/reuse ordering");
}
