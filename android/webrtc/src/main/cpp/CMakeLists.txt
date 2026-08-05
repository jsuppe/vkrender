cmake_minimum_required(VERSION 3.22)
project(vkrender_webrtc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (this file lives at android/webrtc/src/main/cpp)
get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)

set(GST_ANDROID ${REPO}/android/gstreamer-android/${ANDROID_ABI})
if(NOT EXISTS ${GST_ANDROID}/lib/pkgconfig/gstreamer-1.0.pc)
    message(FATAL_ERROR "GStreamer Android prebuilts for ${ANDROID_ABI} not found at "
                        "${GST_ANDROID}. Run scripts/fetch_gstreamer_android.sh.")
endif()

if(CMAKE_HOST_WIN32 AND NOT DEFINED PKG_CONFIG_EXECUTABLE)
    foreach(_pc "C:/Program Files/gstreamer/1.0/msvc_x86_64/bin/pkg-config.exe"
                "C:/gstreamer/1.0/msvc_x86_64/bin/pkg-config.exe")
        if(EXISTS "${_pc}")
            set(PKG_CONFIG_EXECUTABLE "${_pc}")
        endif()
    endforeach()
endif()
find_package(PkgConfig REQUIRED)
if(CMAKE_HOST_WIN32)
    set(ENV{PKG_CONFIG_LIBDIR}
        "${GST_ANDROID}/lib/pkgconfig;${GST_ANDROID}/lib/gstreamer-1.0/pkgconfig")
else()
    set(ENV{PKG_CONFIG_LIBDIR}
        "${GST_ANDROID}/lib/pkgconfig:${GST_ANDROID}/lib/gstreamer-1.0/pkgconfig")
endif()
set(ENV{PKG_CONFIG_ALLOW_SYSTEM_LIBS} 1)
set(ENV{PKG_CONFIG_ALLOW_SYSTEM_CFLAGS} 1)

# The WHIP receive pipeline: whipserversrc (rswebrtc) -> webrtcbin (webrtc) +
# nice/dtls/srtp/rtpmanager(bad)/rsrtp/webrtchttp, then rtp depay + h264parse.
pkg_check_modules(GSTA REQUIRED
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
    gstcoreelements gstapp gstrtp gstrtpmanager gstudp gsttypefindfunctions
    gstvideoparsersbad
    gstwebrtc gstnice gstdtls gstsrtp gstrtpmanagerbad gstrswebrtc gstrsrtp
    gstwebrtchttp)

add_library(vkrender_webrtc SHARED
    webrtc_jni.cpp
    # Reuse the M18 MediaCodec-into-Surface sink (H.264 AUs).
    ${REPO}/android/airplay/src/main/cpp/airplay_video.cpp)
target_include_directories(vkrender_webrtc PRIVATE
    ${REPO}/android/airplay/src/main/cpp
    ${GSTA_STATIC_INCLUDE_DIRS})

list(TRANSFORM GSTA_STATIC_LIBRARIES PREPEND "-l")
target_link_directories(vkrender_webrtc PRIVATE
    ${GSTA_STATIC_LIBRARY_DIRS}
    ${GST_ANDROID}/lib
    ${GST_ANDROID}/lib/gstreamer-1.0)
target_link_options(vkrender_webrtc PRIVATE
    "-Wl,--start-group" ${GSTA_STATIC_LIBRARIES} "-Wl,--end-group")

target_link_libraries(vkrender_webrtc PRIVATE
    android log mediandk nativewindow z)
