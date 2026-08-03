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

# GStreamer Android universal prebuilts (M9 transport/demux). Fetch the
# official tarball and extract to android/gstreamer-android/<abi>/
# (see scripts/fetch_gstreamer_android notes in docs/ANDROID.md).
set(GST_ANDROID ${REPO}/android/gstreamer-android/${ANDROID_ABI})
if(NOT EXISTS ${GST_ANDROID}/lib/pkgconfig/gstreamer-1.0.pc)
    message(FATAL_ERROR "GStreamer Android prebuilts for ${ANDROID_ABI} not found at "
                        "${GST_ANDROID}. Extract the universal tarball there.")
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
pkg_check_modules(GSTA REQUIRED
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
    gstcoreelements gstapp gstrtsp gstrtp gstrtpmanager gstudp gsttcp
    gstplayback gsttypefindfunctions gstvideoparsersbad)

add_library(vkrender_native SHARED
    ${REPO}/decoder/gst_surface_decoder.cpp
    ${REPO}/renderer/vk_context.cpp
    ${REPO}/renderer/external_video_image.cpp
    ${REPO}/renderer/video_pass.cpp
    ${REPO}/renderer/android_main.cpp
    decoder_jni.cpp
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(vkrender_native PRIVATE
    ${REPO}/common ${REPO}/decoder ${REPO}/renderer
    ${GSTA_STATIC_INCLUDE_DIRS}
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Static gstreamer archives have circular deps; group them for the linker.
list(TRANSFORM GSTA_STATIC_LIBRARIES PREPEND "-l")
target_link_directories(vkrender_native PRIVATE ${GSTA_STATIC_LIBRARY_DIRS})
target_link_options(vkrender_native PRIVATE
    "-Wl,--start-group" ${GSTA_STATIC_LIBRARIES} "-Wl,--end-group")

target_link_libraries(vkrender_native PRIVATE
    volk
    android log mediandk nativewindow z)

# Keep the NativeActivity entry point alive despite static linking.
target_link_options(vkrender_native PRIVATE -u ANativeActivity_onCreate)
