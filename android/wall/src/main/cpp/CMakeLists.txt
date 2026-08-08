cmake_minimum_required(VERSION 3.22)
project(vkrender_wall LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (this file lives at android/wall/src/main/cpp)
get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)

# volk loads Vulkan at runtime, which is what the rest of the project uses on
# Android too - no libvulkan link, no version skew with the platform loader.
include(FetchContent)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk
    GIT_TAG 1.3.270
    GIT_SHALLOW TRUE)
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
    GIT_TAG v1.3.270
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(vulkan_headers)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(volk)

add_library(vkrender_wall SHARED wall_jni.cpp)

target_include_directories(vkrender_wall PRIVATE
    ${REPO}/renderer                       # scene_math.h (header-only)
    ${vulkan_headers_SOURCE_DIR}/include
    ${volk_SOURCE_DIR})

target_compile_definitions(vkrender_wall PRIVATE
    VK_USE_PLATFORM_ANDROID_KHR
    VK_NO_PROTOTYPES)

target_sources(vkrender_wall PRIVATE ${volk_SOURCE_DIR}/volk.c)

target_link_libraries(vkrender_wall PRIVATE android log nativewindow)
