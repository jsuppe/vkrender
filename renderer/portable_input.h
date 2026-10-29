#pragma once
#include "framelink.h"
#include "external_video_image.h"
#include "video_pass.h"
#include <cstdlib>
#include <sstream>
namespace vkr {
inline flFormat inputFormat(unsigned index,flFormat fallback) {
    const char* value=std::getenv("VKR_INPUT_FORMATS");if(!value)return fallback;
    std::istringstream list(value);std::string item;
    for(unsigned i=0;i<=index;i++)if(!std::getline(list,item,','))return fallback;
    if(item.empty())return fallback;
    return item=="nv12"?FL_FORMAT_NV12:item=="rgba"?FL_FORMAT_RGBA8:
           item=="bgra"?FL_FORMAT_BGRA8:item=="rgb10a2"?FL_FORMAT_RGB10A2:(flFormat)0;
}
// Explicit portable CPU conversion fallback for formats the native importer
// cannot consume. Source readiness and destination reuse remain caller-owned.
bool uploadFrame(VkContext&,ExternalVideoPool&,VideoPass&,bool& passBound,
                 flImage*,uint32_t key,const flChannelInfo&);
}
