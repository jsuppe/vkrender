#include "android_preview.h"
#include <algorithm>
#include <cstring>
namespace vkr {
namespace {
uint32_t memoryType(VkContext& vk,uint32_t bits,VkMemoryPropertyFlags flags){
 VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(vk.phys,&props);
 for(uint32_t i=0;i<props.memoryTypeCount;i++)if((bits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags)return i;
 return UINT32_MAX;
}
void barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout before,VkImageLayout after){
 VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
 b.srcStageMask=b.dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
 b.srcAccessMask=b.dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
 b.oldLayout=before;b.newLayout=after;b.image=image;
 b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
 b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};d.imageMemoryBarrierCount=1;d.pImageMemoryBarriers=&b;vkCmdPipelineBarrier2(cmd,&d);
}
}
bool AndroidPreview::supported(VkContext& vk){
 const auto f=vk.swapFormat;
 bool rgb=f==VK_FORMAT_B8G8R8A8_UNORM || f==VK_FORMAT_R8G8B8A8_UNORM || f==VK_FORMAT_B8G8R8A8_SRGB || f==VK_FORMAT_R8G8B8A8_SRGB;
 VkFormatProperties p{};vkGetPhysicalDeviceFormatProperties(vk.phys,f,&p);
 bool ok=rgb && (p.optimalTilingFeatures&(VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT))==(VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT);
 mailbox_.supported(ok);return ok;
}
void AndroidPreview::clear(VkContext& vk,Frame& f){
 if(f.source)vkDestroyImage(vk.device,f.source,nullptr);
 if(f.sourceMemory)vkFreeMemory(vk.device,f.sourceMemory,nullptr);
 if(f.mapped)vkUnmapMemory(vk.device,f.memory);
 if(f.buffer)vkDestroyBuffer(vk.device,f.buffer,nullptr);
 if(f.memory)vkFreeMemory(vk.device,f.memory,nullptr);
 if(f.image)vkDestroyImage(vk.device,f.image,nullptr);
 if(f.imageMemory)vkFreeMemory(vk.device,f.imageMemory,nullptr);
 f={};
}
bool AndroidPreview::allocate(VkContext& vk,Frame& f,VkExtent2D extent,VkExtent2D sourceExtent,VkFormat format){
 if(f.image && f.extent.width==extent.width && f.extent.height==extent.height && f.format==format && f.sourceExtent.width==sourceExtent.width && f.sourceExtent.height==sourceExtent.height)return true;
 clear(vk,f);auto fail=[&]{clear(vk,f);return false;};
 VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};im.imageType=VK_IMAGE_TYPE_2D;im.format=format;im.extent={extent.width,extent.height,1};
 im.mipLevels=im.arrayLayers=1;im.samples=VK_SAMPLE_COUNT_1_BIT;im.tiling=VK_IMAGE_TILING_OPTIMAL;im.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
 if(vkCreateImage(vk.device,&im,nullptr,&f.image)!=VK_SUCCESS)return fail();
 VkMemoryRequirements req{};vkGetImageMemoryRequirements(vk.device,f.image,&req);
 VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=req.size;a.memoryTypeIndex=memoryType(vk,req.memoryTypeBits,0);
 if(a.memoryTypeIndex==UINT32_MAX || vkAllocateMemory(vk.device,&a,nullptr,&f.imageMemory)!=VK_SUCCESS || vkBindImageMemory(vk.device,f.image,f.imageMemory,0)!=VK_SUCCESS)return fail();
 im.extent={sourceExtent.width,sourceExtent.height,1};
 if(vkCreateImage(vk.device,&im,nullptr,&f.source)!=VK_SUCCESS)return fail();
 vkGetImageMemoryRequirements(vk.device,f.source,&req);a.allocationSize=req.size;a.memoryTypeIndex=memoryType(vk,req.memoryTypeBits,0);
 if(a.memoryTypeIndex==UINT32_MAX || vkAllocateMemory(vk.device,&a,nullptr,&f.sourceMemory)!=VK_SUCCESS || vkBindImageMemory(vk.device,f.source,f.sourceMemory,0)!=VK_SUCCESS)return fail();
 f.sourceExtent=sourceExtent;
 VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=(VkDeviceSize)extent.width*extent.height*4;b.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 if(vkCreateBuffer(vk.device,&b,nullptr,&f.buffer)!=VK_SUCCESS)return fail();
 vkGetBufferMemoryRequirements(vk.device,f.buffer,&req);a.allocationSize=req.size;a.memoryTypeIndex=memoryType(vk,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 if(a.memoryTypeIndex==UINT32_MAX || vkAllocateMemory(vk.device,&a,nullptr,&f.memory)!=VK_SUCCESS || vkBindBufferMemory(vk.device,f.buffer,f.memory,0)!=VK_SUCCESS || vkMapMemory(vk.device,f.memory,0,VK_WHOLE_SIZE,0,&f.mapped)!=VK_SUCCESS)return fail();
 f.extent=extent;f.format=format;return true;
}
void AndroidPreview::record(VkContext& vk,unsigned slot,VkCommandBuffer cmd,VkImage image,VkExtent2D extent,VkFormat format,VkImageLayout layout){
 auto& f=frames_[slot%2];if(f.pending || f.armed)return;
 uint64_t epoch=mailbox_.takeRequest();if(!epoch || !extent.width || !extent.height)return;
 double scale=std::min(1.,std::min(320./extent.width,180./extent.height));
 VkExtent2D small{std::max(1u,(uint32_t)(extent.width*scale)),std::max(1u,(uint32_t)(extent.height*scale))};
 if(!allocate(vk,f,small,extent,format)){mailbox_.supported(false);return;}
 // Both barriers always supply memory dependencies, including GENERAL sources.
 barrier(cmd,image,layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
 // Copy the presentable image before scaling (also avoids direct WSI blit issues).
 barrier(cmd,f.source,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
 VkImageCopy sourceCopy{};sourceCopy.srcSubresource=sourceCopy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};sourceCopy.extent={extent.width,extent.height,1};
 vkCmdCopyImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&sourceCopy);
 barrier(cmd,f.source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
 barrier(cmd,f.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
 VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
 blit.srcOffsets[1]={(int32_t)extent.width,(int32_t)extent.height,1};blit.dstOffsets[1]={(int32_t)small.width,(int32_t)small.height,1};
 vkCmdBlitImage(cmd,f.source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
 barrier(cmd,f.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
 VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={small.width,small.height,1};
 vkCmdCopyImageToBuffer(cmd,f.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,f.buffer,1,&copy);
 VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
 host.srcStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT;host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
 host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT;host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
 host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=f.buffer;host.size=VK_WHOLE_SIZE;
 VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dependency.bufferMemoryBarrierCount=1;dependency.pBufferMemoryBarriers=&host;
 vkCmdPipelineBarrier2(cmd,&dependency);
 barrier(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,layout);
 f.epoch=epoch;f.armed=true;
}
void AndroidPreview::submitted(unsigned slot){auto& f=frames_[slot%2];if(f.armed){f.pending=true;f.armed=false;}}
void AndroidPreview::retire(unsigned slot){
 auto& f=frames_[slot%2];if(!f.pending)return;f.pending=false;
 std::vector<uint32_t> argb((size_t)f.extent.width*f.extent.height);
 auto* bytes=(const uint8_t*)f.mapped;bool bgra=f.format==VK_FORMAT_B8G8R8A8_UNORM || f.format==VK_FORMAT_B8G8R8A8_SRGB;
 for(size_t i=0;i<argb.size();i++){auto* p=bytes+i*4;argb[i]=0xff000000u|((uint32_t)p[bgra?2:0]<<16)|((uint32_t)p[1]<<8)|p[bgra?0:2];}
 mailbox_.publish(f.epoch,f.extent.width,f.extent.height,std::move(argb));
}
void AndroidPreview::destroy(VkContext& vk){mailbox_.ready(false);for(auto& f:frames_)clear(vk,f);}
}
