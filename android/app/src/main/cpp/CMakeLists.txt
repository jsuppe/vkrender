cmake_minimum_required(VERSION 3.22)
project(vkrender_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (this file lives at android/app/src/main/cpp)
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

# Minimal static FFmpeg (RTSP demux only), prebuilt by scripts/build_ffmpeg_android.sh
set(FFMPEG_ROOT ${REPO}/android/ffmpeg-android/${ANDROID_ABI})
if(NOT EXISTS ${FFMPEG_ROOT}/lib/libavformat.a)
    message(FATAL_ERROR "FFmpeg for ${ANDROID_ABI} not found at ${FFMPEG_ROOT}. "
                        "Run scripts/build_ffmpeg_android.sh on a Linux box first.")
endif()

add_library(vkrender_native SHARED
    ${REPO}/decoder/surface_decoder.cpp
    ${REPO}/renderer/vk_context.cpp
    ${REPO}/renderer/external_video_image.cpp
    ${REPO}/renderer/video_pass.cpp
    ${REPO}/renderer/android_main.cpp
    decoder_jni.cpp
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(vkrender_native PRIVATE
    ${REPO}/common ${REPO}/decoder ${REPO}/renderer
    ${FFMPEG_ROOT}/include
    ${ANDROID_NDK}/sources/android/native_app_glue)

target_link_libraries(vkrender_native PRIVATE
    volk
    ${FFMPEG_ROOT}/lib/libavformat.a
    ${FFMPEG_ROOT}/lib/libavcodec.a
    ${FFMPEG_ROOT}/lib/libavutil.a
    android log mediandk nativewindow z)

# Keep the NativeActivity entry point alive despite static linking.
target_link_options(vkrender_native PRIVATE -u ANativeActivity_onCreate)
