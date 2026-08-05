cmake_minimum_required(VERSION 3.24)
project(vkrender LANGUAGES C CXX)

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
    ${CMAKE_SOURCE_DIR}/renderer/shaders/nv12_planar.frag)
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
    renderer/dump_util.cpp)
target_link_libraries(vkrenderer PRIVATE vkr_common volk glfw)
add_dependencies(vkrenderer vkr_shaders)

# --- Wayland compositor mode (M13, Linux only) -------------------------------
# vkrenderer --wayland: a minimal Wayland server so ordinary clients
# (gstreamer waylandsink, mpv, ...) are zero-copy producers. Protocol glue is
# generated from the vendored XMLs by wayland-scanner.
if(NOT WIN32)
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
        target_include_directories(vkr_wl_dmabuf_client PRIVATE ${VKR_WL_GEN_DIR})
        target_link_libraries(vkr_wl_dmabuf_client PRIVATE PkgConfig::WLCLIENT)
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
