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

# M40: framelink - the renderer consumes vkrender.in.0..11 slot channels
# beside the AIDL Surface API. Sibling checkout, like the desktop builds.
set(FRAMELINK_DIR "${REPO}/../framelink" CACHE PATH "Path to the framelink checkout")
if(NOT EXISTS "${FRAMELINK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "framelink not found at '${FRAMELINK_DIR}'")
endif()
set(FRAMELINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${FRAMELINK_DIR}" "${CMAKE_BINARY_DIR}/framelink")

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
