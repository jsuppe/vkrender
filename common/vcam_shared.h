// M24: shared-frame transport between vkr_vcam (host app, user session) and
// the vkrender camera media source (running inside the Windows Camera Frame
// Server service, session 0).
//
// Direction matters: neither process can OpenProcess the other for handle
// duplication across the user/LocalService boundary, but LocalService HOLDS
// SeCreateGlobalPrivilege - so the MEDIA SOURCE creates the Global\ objects
// (with a permissive DACL) and the host merely opens them by name.
//
// Layout: header + tightly-packed BGRA pixels (width*4 stride). The host
// writes frame + bumps frameCounter under the mutex; the source copies out
// under the mutex when the counter advances. When no host is feeding, the
// camera shows the sample's built-in test pattern (useful diagnostic).
#pragma once

#include <cstdint>

#define VKR_VCAM_SHM L"Global\\vkrender.vcam.frame"
#define VKR_VCAM_MUTEX L"Global\\vkrender.vcam.mutex"

namespace vkr {

constexpr uint32_t kVcamMagic = 0x4D414356; // 'VCAM'
constexpr uint32_t kVcamMaxWidth = 1920;
constexpr uint32_t kVcamMaxHeight = 1080;

struct VcamHeader {
    uint32_t magic;
    uint32_t width;  // written by the host; 0 = no feed yet
    uint32_t height;
    uint32_t _pad;
    uint64_t frameCounter; // bumped by the host after each pixel write
    // BGRA pixels follow, width*4 stride, height rows.
};

constexpr size_t kVcamShmSize =
    sizeof(VcamHeader) + (size_t)kVcamMaxWidth * kVcamMaxHeight * 4;

} // namespace vkr
