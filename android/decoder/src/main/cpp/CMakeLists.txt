cmake_minimum_required(VERSION 3.22)
project(vkrender_decoder LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root (this file lives at android/decoder/src/main/cpp)
get_filename_component(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)

# GStreamer Android universal prebuilts (M9 transport/demux). Fetch with
# scripts/fetch_gstreamer_android.sh or extract the official tarball to
# android/gstreamer-android/<abi>/ (see docs/ANDROID.md).
set(GST_ANDROID ${REPO}/android/gstreamer-android/${ANDROID_ABI})
if(NOT EXISTS ${GST_ANDROID}/lib/pkgconfig/gstreamer-1.0.pc)
    message(FATAL_ERROR "GStreamer Android prebuilts for ${ANDROID_ABI} not found at "
                        "${GST_ANDROID}. Run scripts/fetch_gstreamer_android.sh or extract "
                        "the universal tarball there.")
endif()

# Host pkg-config: on Windows dev machines, reuse the one from the GStreamer
# desktop devel install.
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
pkg_check_modules(GSTA REQUIRED
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
    gstcoreelements gstapp gstrtsp gstrtp gstrtpmanager gstudp gsttcp
    gstplayback gsttypefindfunctions gstvideoparsersbad)

# The decoder app has NO Vulkan dependency: it decodes into the Surface the
# renderer app hands it over Binder.
add_library(vkrender_decoder SHARED
    ${REPO}/decoder/gst_surface_decoder.cpp
    decoder_jni.cpp)

target_include_directories(vkrender_decoder PRIVATE
    ${REPO}/common ${REPO}/decoder
    ${GSTA_STATIC_INCLUDE_DIRS})

# Static gstreamer archives have circular deps; group them for the linker.
list(TRANSFORM GSTA_STATIC_LIBRARIES PREPEND "-l")
target_link_directories(vkrender_decoder PRIVATE
    ${GSTA_STATIC_LIBRARY_DIRS}
    ${GST_ANDROID}/lib
    ${GST_ANDROID}/lib/gstreamer-1.0)
target_link_options(vkrender_decoder PRIVATE
    "-Wl,--start-group" ${GSTA_STATIC_LIBRARIES} "-Wl,--end-group")

target_link_libraries(vkrender_decoder PRIVATE
    android log mediandk nativewindow z)
