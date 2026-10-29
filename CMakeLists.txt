cmake_minimum_required(VERSION 3.24)
project(vkrender LANGUAGES C CXX)

option(VKR_DISABLE_TRACE "Compile out performance tracing" OFF)
if(VKR_DISABLE_TRACE)
    add_compile_definitions(FL_TRACE_COMPILED_OUT)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

include(FetchContent)

# --- Vulkan headers + loader shim (no SDK install required) ------------------
FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG vulkan-sdk-1.3.296.0)
FetchContent_Declare(volk
    GIT_REPOSITORY https://github.com/zeux/volk.git
    GIT_TAG vulkan-sdk-1.3.296.0)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(vulkan_headers volk)
target_link_libraries(volk PUBLIC Vulkan::Headers)
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()

# --- GLFW --------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4)
FetchContent_MakeAvailable(glfw)

# --- glslang (SPIR-V compiler for build-time shader compilation) -------------
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
    GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
    GIT_TAG vulkan-sdk-1.3.296.0)
FetchContent_MakeAvailable(glslang)

# --- Media pipeline (M9: GStreamer everywhere) -------------------------------
if(WIN32)
    # Official GStreamer MSVC runtime + development MSIs must be installed
    # (https://gstreamer.freedesktop.org/download/). The devel MSI sets
    # GSTREAMER_1_0_ROOT_MSVC_X86_64.
    set(GST_ROOT "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}")
    foreach(_cand "C:/gstreamer/1.0/msvc_x86_64"
                  "C:/Program Files/gstreamer/1.0/msvc_x86_64")
        if(NOT GST_ROOT AND EXISTS "${_cand}/lib/gstreamer-1.0.lib")
            set(GST_ROOT "${_cand}")
        endif()
    endforeach()
    if(NOT EXISTS "${GST_ROOT}/lib/gstreamer-1.0.lib")
        message(FATAL_ERROR "GStreamer MSVC (runtime + development) not found at "
                            "'${GST_ROOT}'. Install both MSIs from "
                            "https://gstreamer.freedesktop.org/download/ and re-run CMake.")
    endif()
    add_library(GStreamer::win INTERFACE IMPORTED)
    target_include_directories(GStreamer::win INTERFACE
        "${GST_ROOT}/include/gstreamer-1.0"
        "${GST_ROOT}/include/glib-2.0"
        "${GST_ROOT}/lib/glib-2.0/include"
        "${GST_ROOT}/lib/gstreamer-1.0/include") # generated per-arch headers
    target_link_directories(GStreamer::win INTERFACE "${GST_ROOT}/lib")
    target_link_libraries(GStreamer::win INTERFACE
        gstreamer-1.0.lib gstapp-1.0.lib gstvideo-1.0.lib gstd3d11-1.0.lib
        gobject-2.0.lib glib-2.0.lib)
else()
    find_package(PkgConfig REQUIRED)
    # M9: GStreamer end-to-end decode pipeline (rtspsrc -> decodebin3 -> va
    # decoders); libva stays for the surface export call.
    pkg_check_modules(GST REQUIRED IMPORTED_TARGET
        gstreamer-1.0>=1.22 gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-va-1.0)
    pkg_check_modules(LIBVA REQUIRED IMPORTED_TARGET libva>=1.16 libva-drm)
endif()

# --- common ------------------------------------------------------------------
add_library(vkr_common STATIC
    $<$<BOOL:${WIN32}>:common/ipc_win32.cpp>
    $<$<NOT:$<BOOL:${WIN32}>>:common/ipc_posix.cpp>)
target_include_directories(vkr_common PUBLIC common)

# --- decoder -----------------------------------------------------------------
add_executable(vkdecoder decoder/main.cpp)
target_link_libraries(vkdecoder PRIVATE vkr_common)
if(WIN32)
    target_sources(vkdecoder PRIVATE decoder/gst_d3d11_decoder.cpp)
    target_link_libraries(vkdecoder PRIVATE GStreamer::win d3d11 dxgi)
else()
    target_sources(vkdecoder PRIVATE decoder/gst_va_decoder.cpp)
    target_link_libraries(vkdecoder PRIVATE PkgConfig::GST PkgConfig::LIBVA)
endif()

# --- shader compilation ------------------------------------------------------
set(VKR_SHADERS
    ${CMAKE_SOURCE_DIR}/renderer/shaders/fullscreen.vert
    ${CMAKE_SOURCE_DIR}/renderer/shaders/cube.vert
    ${CMAKE_SOURCE_DIR}/renderer/shaders/vaporwave.frag
    ${CMAKE_SOURCE_DIR}/renderer/shaders/video_ycbcr.frag
    ${CMAKE_SOURCE_DIR}/renderer/shaders/nv12_planar.frag
    ${CMAKE_SOURCE_DIR}/renderer/shaders/nv12_cube.frag)
set(VKR_SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${VKR_SHADER_OUT_DIR})
set(VKR_SHADER_OUTPUTS "")
foreach(_shader ${VKR_SHADERS})
    get_filename_component(_name ${_shader} NAME)
    set(_out ${VKR_SHADER_OUT_DIR}/${_name}.spv)
    add_custom_command(OUTPUT ${_out}
        COMMAND $<TARGET_FILE:glslang-standalone> -V -o ${_out} ${_shader}
        DEPENDS ${_shader} glslang-standalone
        COMMENT "Compiling ${_name} -> SPIR-V")
    list(APPEND VKR_SHADER_OUTPUTS ${_out})
endforeach()
add_custom_target(vkr_shaders DEPENDS ${VKR_SHADER_OUTPUTS})

# --- renderer ----------------------------------------------------------------
add_executable(vkrenderer
    renderer/main.cpp
    renderer/vk_context.cpp
    renderer/external_video_image.cpp
    renderer/video_pass.cpp
    renderer/background_pass.cpp
    renderer/nv12_output.cpp
    renderer/portable_output.cpp
    renderer/portable_input.cpp
    renderer/dump_util.cpp
    renderer/scene_host.cpp)
if(NOT WIN32)
    target_sources(vkrenderer PRIVATE renderer/gpu_output.cpp)
endif()
target_link_libraries(vkrenderer PRIVATE vkr_common volk glfw)
add_dependencies(vkrenderer vkr_shaders)
target_sources(vkrenderer PRIVATE renderer/control_api.cpp third_party/civetweb/civetweb.c)
target_include_directories(vkrenderer PRIVATE third_party/civetweb third_party/stb)
target_compile_definitions(vkrenderer PRIVATE NO_SSL USE_IPV6)
if(WIN32)
    # M14: producer-facing service mode (--serve). M38 retired the client
    # library that used to come with it: producers speak framelink now.
    # M42: + the moderation API (civetweb compiled NO_SSL - localhost only,
    # so no TLS and no OpenSSL dependency in the renderer).
    # Target-level NO_SSL, deliberately NOT set_source_files_properties: source
    # properties are directory-scoped and would strip TLS from vkr_web's build
    # of the very same civetweb.c.
    target_sources(vkrenderer PRIVATE renderer/win_service.cpp)
    target_include_directories(vkrenderer PRIVATE third_party/civetweb third_party/stb)
    target_compile_definitions(vkrenderer PRIVATE NO_SSL USE_IPV6)
    target_link_libraries(vkrenderer PRIVATE d3d11 dxgi framelink ws2_32)

    # framelink is its own project now (github: framelink, MIT). vkrender is a
    # CONSUMER of it - the virtual camera owns a channel and the renderer
    # attaches as a producer - which is the whole point of extracting it.
    # FRAMELINK_DIR overrides the sibling-checkout default.
    set(FRAMELINK_DIR "${CMAKE_SOURCE_DIR}/../framelink" CACHE PATH
        "Path to the framelink checkout")
    if(NOT EXISTS "${FRAMELINK_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "framelink not found at '${FRAMELINK_DIR}'. Clone it beside vkrender "
            "or pass -DFRAMELINK_DIR=<path>.")
    endif()
    set(FRAMELINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${FRAMELINK_DIR}" "${CMAKE_BINARY_DIR}/framelink")

    # Sub-projects default to their own binary dirs, which would leave
    # framelink.dll somewhere the appliance cannot load it and the installer
    # cannot package it. Put every sub-project artefact beside vkrender's.
    function(vkr_colocate)
        foreach(_t ${ARGN})
            if(TARGET ${_t})
                set_target_properties(${_t} PROPERTIES
                    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/Debug"
                    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
                    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/RelWithDebInfo")
            endif()
        endforeach()
    endfunction()
    vkr_colocate(framelink)

    # M25: the appliance launcher - starts the renderer + every receiver +
    # the virtual camera as one kill-on-close group, configured by
    # vkrender.ini (what the installer's Start Menu shortcut runs).
    add_executable(vkr_appliance client/appliance_win.cpp)
    target_compile_options(vkr_appliance PRIVATE /EHsc)

    # M36: the virtual camera is its own project now (framecam, MIT), because
    # it is a framelink integration rather than a part of this compositor: it
    # creates the camera and hands out buffers, and vkrender's renderer is just
    # the first application to render into them.
    #
    # Optional, unlike framelink: vkrender does not LINK the camera, it only
    # launches it. A checkout beside vkrender gets built so the appliance and
    # installer keep working; without one, everything else still builds.
    set(FRAMECAM_DIR "${CMAKE_SOURCE_DIR}/../framecam" CACHE PATH
        "Path to the framecam checkout (optional)")
    if(EXISTS "${FRAMECAM_DIR}/CMakeLists.txt")
        add_subdirectory("${FRAMECAM_DIR}" "${CMAKE_BINARY_DIR}/framecam")
    else()
        message(STATUS "framecam not found at '${FRAMECAM_DIR}' - skipping the "
                       "virtual camera. Clone it beside vkrender to build it.")
    endif()
    vkr_colocate(framecam softcam vkrender_vcam_source)

    # M38: a framelink producer now, not a vkrender_client one.
    add_executable(vkr_client_demo test/win_client_demo.cpp)
    target_link_libraries(vkr_client_demo PRIVATE framelink d3d11)

    # M41: the appliance web front door - serves the browser sender + the
    # desktop-app downloads over HTTPS (getDisplayMedia needs a secure
    # context). civetweb is vendored (MIT); TLS and the self-signed cert
    # bootstrap use the OpenSSL that ships with the GStreamer runtime.
    # applink.c: OpenSSL FILE* BIOs cross the DLL boundary -> must be compiled
    # into the exe (the vkrender-airplay repo learned this first).
    add_executable(vkr_web client/vkr_web.cpp third_party/civetweb/civetweb.c
        "${GST_ROOT}/include/openssl/applink.c")
    target_include_directories(vkr_web PRIVATE third_party/civetweb "${GST_ROOT}/include")
    target_compile_definitions(vkr_web PRIVATE
        NO_SSL_DL OPENSSL_API_3_0 USE_IPV6 NDEBUG)
    target_link_directories(vkr_web PRIVATE "${GST_ROOT}/lib")
    target_link_libraries(vkr_web PRIVATE ssl crypto ws2_32 crypt32)
    target_compile_options(vkr_web PRIVATE /EHsc /Zi)
    target_link_options(vkr_web PRIVATE /DEBUG /MAP)

    # M16: Miracast sink -> producer bridge (C++/WinRT; needs the SDK's
    # cppwinrt projection headers and WindowsApp.lib).
    add_executable(vkr_miracast client/miracast_receiver.cpp client/pin_overlay.cpp)
    target_link_libraries(vkr_miracast PRIVATE framelink d3d11 dxgi windowsapp)
    # C++/WinRT on standard C++20 coroutines; /await is rejected by MSVC 14.5x.
    target_compile_options(vkr_miracast PRIVATE /EHsc)

    # M19: the Windows AirPlay receiver now lives in its own public repository
    # (github.com/jsuppe/vkrender-airplay - GPLv3, vendored UxPlay core). It
    # depends on framelink, not on this repo (M38). The vendored
    # third_party/uxplay + libplist trees stay HERE for the Android AirPlay
    # APK (android/airplay), which shares the same core.

    # M20: WebRTC (WHIP) receiver -> producer. whipserversrc does all the
    # WebRTC work; this is just a decode->ring bridge (shared design with the
    # Linux/Android receivers).
    add_executable(vkr_webrtc client/webrtc_win.cpp client/whip_room.cpp
        client/whip_mdns_win.c client/pin_overlay.cpp)
    target_link_libraries(vkr_webrtc PRIVATE framelink GStreamer::win
        gio-2.0.lib gstsdp-1.0.lib gstwebrtc-1.0.lib d3d11 dxgi dnsapi)
    target_compile_options(vkr_webrtc PRIVATE /EHsc)

    # M27: RTSP multi-source receiver -> producer. Pull-side ingest (we dial
    # out to cameras/encoders); the shared rtsp_room core also drives the
    # Linux and Android receivers. gio for the room's HTTP control API.
    add_executable(vkr_rtsp client/rtsp_win.cpp client/rtsp_room.cpp)
    target_link_libraries(vkr_rtsp PRIVATE framelink GStreamer::win
        gio-2.0.lib d3d11 dxgi)
    target_compile_options(vkr_rtsp PRIVATE /EHsc)

endif()

# --- Wayland compositor mode (M13, Linux only) -------------------------------
# vkrenderer --wayland: a minimal Wayland server so ordinary clients
# (gstreamer waylandsink, mpv, ...) are zero-copy producers. Protocol glue is
# generated from the vendored XMLs by wayland-scanner.
if(NOT WIN32)
    # M39: framelink input slots on Linux too - the compositor consumes them
    # alongside the Wayland socket, and the receivers produce into them.
    set(FRAMELINK_DIR "${CMAKE_SOURCE_DIR}/../framelink" CACHE PATH
        "Path to the framelink checkout")
    if(NOT EXISTS "${FRAMELINK_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "framelink not found at '${FRAMELINK_DIR}'. Clone it beside vkrender "
            "or pass -DFRAMELINK_DIR=<path>.")
    endif()
    if(NOT TARGET framelink)
        set(FRAMELINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${FRAMELINK_DIR}" "${CMAKE_BINARY_DIR}/framelink")
    endif()
    target_link_libraries(vkrenderer PRIVATE framelink)

    pkg_check_modules(WLSERVER REQUIRED IMPORTED_TARGET wayland-server>=1.20)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(VKR_WL_PROTOCOLS
        ${CMAKE_SOURCE_DIR}/third_party/wayland-protocols/xdg-shell.xml
        ${CMAKE_SOURCE_DIR}/third_party/wayland-protocols/linux-dmabuf-v1.xml)
    set(VKR_WL_GEN_DIR ${CMAKE_BINARY_DIR}/wayland-gen)
    file(MAKE_DIRECTORY ${VKR_WL_GEN_DIR})
    set(VKR_WL_GEN_SOURCES "")
    foreach(_proto ${VKR_WL_PROTOCOLS})
        get_filename_component(_pname ${_proto} NAME_WE)
        set(_hdr ${VKR_WL_GEN_DIR}/${_pname}-server-protocol.h)
        set(_src ${VKR_WL_GEN_DIR}/${_pname}-protocol.c)
        add_custom_command(OUTPUT ${_hdr} ${_src}
            COMMAND ${WAYLAND_SCANNER} server-header ${_proto} ${_hdr}
            COMMAND ${WAYLAND_SCANNER} private-code ${_proto} ${_src}
            DEPENDS ${_proto}
            COMMENT "wayland-scanner ${_pname}")
        # Header listed as a source too so it exists before the .cpp compiles.
        list(APPEND VKR_WL_GEN_SOURCES ${_src} ${_hdr})
    endforeach()
    target_sources(vkrenderer PRIVATE renderer/wayland_compositor.cpp ${VKR_WL_GEN_SOURCES})
    target_include_directories(vkrenderer PRIVATE ${VKR_WL_GEN_DIR})
    target_link_libraries(vkrenderer PRIVATE PkgConfig::WLSERVER)

    # Test client: feeds the compositor a udmabuf-backed ARGB dma-buf through
    # zwp_linux_dmabuf (client-side protocol glue generated here too).
    pkg_check_modules(WLCLIENT IMPORTED_TARGET wayland-client>=1.20)
    if(WLCLIENT_FOUND)
        set(VKR_WL_CLIENT_GEN "")
        foreach(_proto ${VKR_WL_PROTOCOLS})
            get_filename_component(_pname ${_proto} NAME_WE)
            set(_chdr ${VKR_WL_GEN_DIR}/${_pname}-client-protocol.h)
            add_custom_command(OUTPUT ${_chdr}
                COMMAND ${WAYLAND_SCANNER} client-header ${_proto} ${_chdr}
                DEPENDS ${_proto}
                COMMENT "wayland-scanner client ${_pname}")
            list(APPEND VKR_WL_CLIENT_GEN ${_chdr} ${VKR_WL_GEN_DIR}/${_pname}-protocol.c)
        endforeach()
        add_executable(vkr_wl_dmabuf_client test/wayland_dmabuf_client.c ${VKR_WL_CLIENT_GEN})
        target_link_libraries(vkr_wl_dmabuf_client PRIVATE framelink)
        target_include_directories(vkr_wl_dmabuf_client PRIVATE ${VKR_WL_GEN_DIR})
        target_link_libraries(vkr_wl_dmabuf_client PRIVATE PkgConfig::WLCLIENT)
    endif()

    # M22: multi-presenter WHIP room -> waylandsink into the compositor.
    # webrtcbin/SDP come from gst-plugins-bad; gio for the room's HTTP listener.
    pkg_check_modules(GSTRTC IMPORTED_TARGET
        gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gio-2.0)
    if(GSTRTC_FOUND)
        add_executable(vkr_webrtc client/webrtc_linux.cpp client/whip_room.cpp)
        target_link_libraries(vkr_webrtc PRIVATE PkgConfig::GST PkgConfig::GSTRTC framelink)
    else()
        message(STATUS "gstreamer-webrtc-1.0 not found - skipping vkr_webrtc (M22)")
    endif()

    # M27: RTSP multi-source receiver -> waylandsink into the compositor. Needs
    # no webrtc/sdp - only gio, for the room's HTTP control API.
    pkg_check_modules(GIO IMPORTED_TARGET gio-2.0)
    if(GIO_FOUND)
        add_executable(vkr_rtsp client/rtsp_linux.cpp client/rtsp_room.cpp)
        target_link_libraries(vkr_rtsp PRIVATE PkgConfig::GST PkgConfig::GIO framelink)
    else()
        message(STATUS "gio-2.0 not found - skipping vkr_rtsp (M27)")
    endif()
endif()
add_custom_command(TARGET vkrenderer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${VKR_SHADER_OUT_DIR} "$<TARGET_FILE_DIR:vkrenderer>/shaders")

# --- tests -------------------------------------------------------------------
if(NOT WIN32)
    enable_testing()
    add_executable(vkr_ipc_test test/ipc_test.cpp)
    target_link_libraries(vkr_ipc_test PRIVATE vkr_common)
    add_test(NAME ipc COMMAND vkr_ipc_test)
endif()

if(WIN32)
    enable_testing()
    add_executable(vkr_nv12_output_test test/nv12_output_win.cpp
        renderer/nv12_output.cpp renderer/vk_context.cpp renderer/external_video_image.cpp)
    target_include_directories(vkr_nv12_output_test PRIVATE renderer)
    target_link_libraries(vkr_nv12_output_test PRIVATE vkr_common volk glfw framelink d3d11 dxgi)
    add_dependencies(vkr_nv12_output_test vkr_shaders)
    foreach(size "64;32" "130;66" "1920;1080")
        list(GET size 0 w)
        list(GET size 1 h)
        add_test(NAME nv12_gpu_${w}x${h} COMMAND vkr_nv12_output_test ${VKR_SHADER_OUT_DIR} ${w} ${h})
        set_tests_properties(nv12_gpu_${w}x${h} PROPERTIES TIMEOUT 60)
    endforeach()
endif()

# Integration driver: run alongside a renderer publishing <prefix>.rgba/.nv12.
add_executable(vkr_portable_output_check test/portable_output_check.cpp)
target_link_libraries(vkr_portable_output_check PRIVATE framelink)

if(UNIX AND NOT ANDROID AND TARGET vkr_wl_dmabuf_client)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    enable_testing()
    add_test(NAME mixed_rgb_nv12_input COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test/mixed_formats.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(mixed_rgb_nv12_input PROPERTIES TIMEOUT 30)
endif()

if(UNIX AND NOT ANDROID)
    add_executable(vkr_gpu_sync_fd_test test/gpu_sync_fd.cpp renderer/vk_context.cpp)
    target_include_directories(vkr_gpu_sync_fd_test PRIVATE renderer common)
    target_link_libraries(vkr_gpu_sync_fd_test PRIVATE framelink volk glfw)
    add_test(NAME vulkan_sync_fd_contract COMMAND vkr_gpu_sync_fd_test)
    set_tests_properties(vulkan_sync_fd_contract PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
endif()

add_executable(vkr_scene_layout_test test/scene_layout.cpp)
target_include_directories(vkr_scene_layout_test PRIVATE renderer common)
target_link_libraries(vkr_scene_layout_test PRIVATE volk)
enable_testing()
add_test(NAME shared_scene_layouts COMMAND vkr_scene_layout_test)

if(UNIX AND NOT ANDROID)
    add_executable(vkr_gpu_output_caps test/gpu_output_caps.cpp renderer/vk_context.cpp)
    target_include_directories(vkr_gpu_output_caps PRIVATE renderer common)
    target_link_libraries(vkr_gpu_output_caps PRIVATE volk glfw)
endif()

if(UNIX AND NOT ANDROID)
 add_executable(vkr_gpu_output_pixels test/gpu_output_pixels.cpp renderer/gpu_output.cpp renderer/vk_context.cpp)
 target_include_directories(vkr_gpu_output_pixels PRIVATE renderer common)
 target_link_libraries(vkr_gpu_output_pixels PRIVATE framelink volk glfw)
 foreach(fmt 1 2 3 4)
  add_test(NAME gpu_output_pixels_${fmt} COMMAND vkr_gpu_output_pixels ${fmt})
  set_tests_properties(gpu_output_pixels_${fmt} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
 endforeach()
endif()

if(UNIX AND NOT ANDROID)
 add_test(NAME gpu_output_pixels_4k_rgba COMMAND vkr_gpu_output_pixels 4 large)
 set_tests_properties(gpu_output_pixels_4k_rgba PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
endif()

# Pure control-plane tests: no Vulkan device or display needed.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_executable(vkr_control_test test/control_fixture.cpp renderer/control_api.cpp third_party/civetweb/civetweb.c)
target_include_directories(vkr_control_test PRIVATE renderer common third_party/civetweb third_party/stb)
target_compile_definitions(vkr_control_test PRIVATE NO_SSL USE_IPV6)
if(WIN32)
    target_link_libraries(vkr_control_test PRIVATE ws2_32)
else()
    target_link_libraries(vkr_control_test PRIVATE pthread dl)
endif()
add_test(NAME shared_moderation COMMAND vkr_control_test)
add_test(NAME control_http COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/control_api.py $<TARGET_FILE:vkr_control_test>)
set_tests_properties(control_http PROPERTIES TIMEOUT 20)
add_test(NAME control_preview_admission COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/control_preview_admission.py $<TARGET_FILE:vkr_control_test>)
set_tests_properties(control_preview_admission PROPERTIES TIMEOUT 20)
# Android Binder scene-generation contract is shared/header-only; exercise it
# on desktop CI without claiming native Binder/GPU lifetime qualification.
add_executable(vkr_android_surface_generations_test test/android_surface_generations.cpp)
target_include_directories(vkr_android_surface_generations_test PRIVATE renderer common)
target_link_libraries(vkr_android_surface_generations_test PRIVATE volk)
add_test(NAME android_surface_generations COMMAND vkr_android_surface_generations_test)
find_package(Java COMPONENTS Runtime Development QUIET)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Java_FOUND AND Python3_Interpreter_FOUND)
    add_test(NAME android_stream_owners COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/android_stream_owners.py)
endif()

add_executable(vkr_android_control_test test/android_control.cpp)
target_include_directories(vkr_android_control_test PRIVATE renderer)
add_test(NAME android_control_mailbox COMMAND vkr_android_control_test)
add_test(NAME android_control_aidl COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test/android_control_aidl.py)
