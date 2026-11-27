cmake_minimum_required(VERSION 3.22)
project(vkrender_renderer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (this file lives at android/renderer/src/main/cpp)
get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)

include(FetchContent)
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.3.296.0)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.3.296.0)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(vulkan_headers volk)
target_link_libraries(volk PUBLIC Vulkan::Headers)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR)

# Optional FrameLink A/V input (FrameLink decision 12, 6a): the GStreamer
# Android SDK architecture directory. Unset, the renderer stays GStreamer-free.
set(VKR_GST_ANDROID_ROOT "" CACHE PATH "GStreamer Android SDK arch dir; enables the A/V input")
set(VKR_GST_ANDROID_UNIVERSAL "" CACHE PATH "GStreamer Android universal SDK (per-ABI subdirectories)")
if(VKR_GST_ANDROID_UNIVERSAL AND NOT VKR_GST_ANDROID_ROOT)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(VKR_GST_ANDROID_ROOT "${VKR_GST_ANDROID_UNIVERSAL}/arm64")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(VKR_GST_ANDROID_ROOT "${VKR_GST_ANDROID_UNIVERSAL}/x86_64")
    endif()
endif()

# M40: framelink - the renderer consumes vkrender.in.0..11 slot channels
# beside the AIDL Surface API. Sibling checkout, like the desktop builds.
include("${REPO}/cmake/FrameLinkDependency.cmake")
vkr_add_framelink("${REPO}")

# The renderer app has NO GStreamer/decode dependency at all: its input is the
# AImageReader Surface it exports over Binder (see the decoder app).
add_library(vkrender_renderer SHARED
    ${REPO}/renderer/vk_context.cpp
    ${REPO}/renderer/external_video_image.cpp
    ${REPO}/renderer/video_pass.cpp
    ${REPO}/renderer/background_pass.cpp
    ${REPO}/renderer/portable_output.cpp
    ${REPO}/renderer/gpu_output.cpp
    ${REPO}/renderer/android_displays.cpp
    ${REPO}/renderer/android_preview.cpp
    ${REPO}/renderer/android_main.cpp
    renderer_jni.cpp
    fltest_producer.cpp
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(vkrender_renderer PRIVATE
    ${REPO}/common ${REPO}/renderer
    ${ANDROID_NDK}/sources/android/native_app_glue)

target_link_libraries(vkrender_renderer PRIVATE
    volk framelink
    android log mediandk nativewindow)

# Keep the NativeActivity entry point alive despite static linking.
target_link_options(vkrender_renderer PRIVATE -u ANativeActivity_onCreate)

# The A/V input: the desktop AvInput over FrameLink's av::runPlayback, with
# FrameLink's static GStreamer element and plugins (its android/avsample set).
# PCM goes to fakesink: the renderer counts and times it but has no audio
# output on Android. A second library runs FrameLink's A/V producer in the
# APK's own ":avproducer" process (same app UID, as abstract sockets need).
if(VKR_GST_ANDROID_ROOT)
    if(NOT TARGET gstframelink)
        message(FATAL_ERROR "VKR_GST_ANDROID_ROOT set but FrameLink built no gstframelink")
    endif()
    find_package(PkgConfig REQUIRED)
    set(ENV{PKG_CONFIG_LIBDIR} "${VKR_GST_ANDROID_ROOT}/lib/pkgconfig:${VKR_GST_ANDROID_ROOT}/lib/gstreamer-1.0/pkgconfig")
    set(ENV{PKG_CONFIG_PATH} "")
    pkg_check_modules(VKR_AV REQUIRED gio-2.0 gstreamer-audio-1.0 gstreamer-net-1.0 gstapp gstcoreelements
        gstgdp gsttcp gstgio gstaudioconvert gstaudioresample gstvolume gstopensles)
    set(VKR_AV_SOURCES ${FRAMELINK_DIR}/samples/av_apps.cpp ${FRAMELINK_DIR}/samples/av_memory_agreement.cpp
        ${FRAMELINK_DIR}/runtime/av_auth.cpp ${FRAMELINK_DIR}/runtime/av_rendezvous.cpp)
    add_library(vkr_av_playback STATIC ${VKR_AV_SOURCES})
    target_compile_definitions(vkr_av_playback PRIVATE AV_PLAYBACK_LIBRARY=1)
    target_include_directories(vkr_av_playback PUBLIC ${FRAMELINK_DIR}/samples ${FRAMELINK_DIR}/android/avsample ${VKR_AV_STATIC_INCLUDE_DIRS})
    target_link_directories(vkr_av_playback PUBLIC ${VKR_AV_STATIC_LIBRARY_DIRS})
    target_link_libraries(vkr_av_playback PUBLIC gstframelink "-Wl,--start-group" ${VKR_AV_STATIC_LIBRARIES} "-Wl,--end-group" OpenSLES)
    target_link_libraries(vkrender_renderer PRIVATE vkr_av_playback)
    target_compile_definitions(vkrender_renderer PRIVATE VKR_ANDROID_AV=1)

    add_library(vkr_av_producer SHARED av_producer_jni.cpp ${VKR_AV_SOURCES})
    target_compile_definitions(vkr_av_producer PRIVATE AV_PRODUCER=1 main=flProducerMain)
    target_include_directories(vkr_av_producer PRIVATE ${FRAMELINK_DIR}/samples ${FRAMELINK_DIR}/android/avsample ${VKR_AV_STATIC_INCLUDE_DIRS})
    target_link_directories(vkr_av_producer PRIVATE ${VKR_AV_STATIC_LIBRARY_DIRS})
    target_link_libraries(vkr_av_producer PRIVATE gstframelink "-Wl,--start-group" ${VKR_AV_STATIC_LIBRARIES} "-Wl,--end-group" OpenSLES android log)
endif()

option(VKR_DISABLE_TRACE "Compile out renderer tracing" OFF)
if(VKR_DISABLE_TRACE)
    target_compile_definitions(vkrender_renderer PRIVATE FL_TRACE_COMPILED_OUT)
endif()

add_executable(vkr_portable_output_check ${REPO}/test/portable_output_check.cpp)
target_link_libraries(vkr_portable_output_check PRIVATE framelink)

add_executable(vkr_gpu_sync_fd_test ${REPO}/test/gpu_sync_fd.cpp ${REPO}/renderer/vk_context.cpp)
target_include_directories(vkr_gpu_sync_fd_test PRIVATE ${REPO}/renderer ${REPO}/common)
target_link_libraries(vkr_gpu_sync_fd_test PRIVATE framelink volk android log nativewindow)

add_executable(vkr_gpu_output_caps ${REPO}/test/gpu_output_caps.cpp ${REPO}/renderer/vk_context.cpp)
target_include_directories(vkr_gpu_output_caps PRIVATE ${REPO}/renderer ${REPO}/common)
target_link_libraries(vkr_gpu_output_caps PRIVATE volk android log nativewindow)

add_executable(vkr_gpu_output_pixels ${REPO}/test/gpu_output_pixels.cpp ${REPO}/renderer/gpu_output.cpp ${REPO}/renderer/vk_context.cpp)
target_include_directories(vkr_gpu_output_pixels PRIVATE ${REPO}/renderer ${REPO}/common)
target_link_libraries(vkr_gpu_output_pixels PRIVATE framelink volk android log nativewindow)
