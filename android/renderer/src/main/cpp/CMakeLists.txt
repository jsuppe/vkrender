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

# The renderer app has NO GStreamer/decode dependency at all: its input is the
# AImageReader Surface it exports over Binder (see the decoder app).
add_library(vkrender_renderer SHARED
    ${REPO}/renderer/vk_context.cpp
    ${REPO}/renderer/external_video_image.cpp
    ${REPO}/renderer/video_pass.cpp
    ${REPO}/renderer/background_pass.cpp
    ${REPO}/renderer/android_main.cpp
    renderer_jni.cpp
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(vkrender_renderer PRIVATE
    ${REPO}/common ${REPO}/renderer
    ${ANDROID_NDK}/sources/android/native_app_glue)

target_link_libraries(vkrender_renderer PRIVATE
    volk
    android log mediandk nativewindow)

# Keep the NativeActivity entry point alive despite static linking.
target_link_options(vkrender_renderer PRIVATE -u ANativeActivity_onCreate)
