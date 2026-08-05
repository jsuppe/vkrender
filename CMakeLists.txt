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
    renderer/dump_util.cpp
    renderer/scene_host.cpp)
target_link_libraries(vkrenderer PRIVATE vkr_common volk glfw)
add_dependencies(vkrenderer vkr_shaders)
if(WIN32)
    # M14: producer-facing service mode (--serve) + client library + demo.
    target_sources(vkrenderer PRIVATE renderer/win_service.cpp)
    target_link_libraries(vkrenderer PRIVATE d3d11 dxgi)

    add_library(vkrender_client SHARED client/vkrender_client.cpp)
    target_include_directories(vkrender_client PUBLIC client)
    target_link_libraries(vkrender_client PRIVATE vkr_common d3d11 dxgi)

    add_executable(vkr_client_demo test/win_client_demo.cpp)
    target_link_libraries(vkr_client_demo PRIVATE vkrender_client d3d11)

    # M16: Miracast sink -> producer bridge (C++/WinRT; needs the SDK's
    # cppwinrt projection headers and WindowsApp.lib).
    add_executable(vkr_miracast client/miracast_receiver.cpp)
    target_link_libraries(vkr_miracast PRIVATE vkrender_client d3d11 dxgi windowsapp)
    # C++/WinRT on standard C++20 coroutines; /await is rejected by MSVC 14.5x.
    target_compile_options(vkr_miracast PRIVATE /EHsc)

    # M19: AirPlay receiver (native Windows producer). Ports the same vendored
    # UxPlay core as the Android APK, under MSVC via a small Win32 pthread shim
    # (third_party/win_compat), with a DnsServiceRegister-based dnssd shim
    # (no Bonjour). OpenSSL comes from the GStreamer MSVC install (GST_ROOT).
    set(UX ${CMAKE_SOURCE_DIR}/third_party/uxplay/lib)
    set(PLIST ${CMAKE_SOURCE_DIR}/third_party/libplist)
    set(WINCOMPAT ${CMAKE_SOURCE_DIR}/third_party/win_compat)

    add_library(vkr_plist_win STATIC
        ${PLIST}/src/base64.c ${PLIST}/src/bytearray.c ${PLIST}/src/bplist.c
        ${PLIST}/src/hashtable.c ${PLIST}/src/jplist.c ${PLIST}/src/jsmn.c
        ${PLIST}/src/oplist.c ${PLIST}/src/plist.c ${PLIST}/src/ptrarray.c
        ${PLIST}/src/time64.c ${PLIST}/src/xplist.c
        ${PLIST}/src/out-default.c ${PLIST}/src/out-limd.c ${PLIST}/src/out-plutil.c
        ${PLIST}/libcnary/cnary.c ${PLIST}/libcnary/node.c ${PLIST}/libcnary/node_list.c)
    target_include_directories(vkr_plist_win PUBLIC
        ${PLIST}/include ${PLIST}/src ${PLIST}/libcnary/include)
    # LIBPLIST_STATIC: the API macro is neither dllimport nor dllexport when
    # linked statically (PUBLIC so the raop lib that includes plist.h agrees).
    target_compile_definitions(vkr_plist_win PUBLIC LIBPLIST_STATIC=1)
    # No HAVE_STRNDUP on MSVC (bionic has it, the UCRT does not) - let the
    # bundled fallback compile.
    target_compile_definitions(vkr_plist_win PRIVATE
        PACKAGE_VERSION="2.6.0" WIN32=1 _CRT_SECURE_NO_WARNINGS)

    add_library(vkr_raop_win STATIC
        ${UX}/byteutils.c ${UX}/crypto.c ${UX}/fairplay_playfair.c
        ${UX}/http_request.c ${UX}/http_response.c ${UX}/httpd.c ${UX}/logger.c
        ${UX}/mirror_buffer.c ${UX}/netutils.c ${UX}/pairing.c ${UX}/raop.c
        ${UX}/raop_buffer.c ${UX}/raop_ntp.c ${UX}/raop_rtp.c
        ${UX}/raop_rtp_mirror.c ${UX}/srp.c ${UX}/utils.c
        ${UX}/llhttp/api.c ${UX}/llhttp/http.c ${UX}/llhttp/llhttp.c
        ${UX}/playfair/hand_garble.c ${UX}/playfair/modified_md5.c
        ${UX}/playfair/omg_hax.c ${UX}/playfair/playfair.c ${UX}/playfair/sap_hash.c
        ${CMAKE_SOURCE_DIR}/client/dnssd_win.c
        ${WINCOMPAT}/pthread.c)
    # win_compat is first so <pthread.h>/<unistd.h> resolve to the shim.
    target_include_directories(vkr_raop_win PUBLIC
        ${WINCOMPAT} ${UX} ${UX}/llhttp ${UX}/playfair
        "${GST_ROOT}/include")  # openssl headers
    target_link_directories(vkr_raop_win PUBLIC "${GST_ROOT}/lib")
    target_link_libraries(vkr_raop_win PUBLIC vkr_plist_win
        crypto.lib ssl.lib dnsapi ws2_32 iphlpapi)
    target_compile_definitions(vkr_raop_win PRIVATE WIN32=1 _CRT_SECURE_NO_WARNINGS)
    # Third-party C: silence its warnings, don't fail the build on them.
    target_compile_options(vkr_raop_win PRIVATE /w)

    # OpenSSL's file BIOs (the FairPlay keyfile) pass CRT FILE* across the DLL
    # boundary, which requires applink.c compiled into the main exe.
    # M20: WebRTC (WHIP) receiver -> producer. whipserversrc does all the
    # WebRTC work; this is just a decode->ring bridge (shared design with the
    # Linux/Android receivers).
    add_executable(vkr_webrtc client/webrtc_win.cpp)
    target_link_libraries(vkr_webrtc PRIVATE vkrender_client GStreamer::win d3d11 dxgi)
    target_compile_options(vkr_webrtc PRIVATE /EHsc)

    add_executable(vkr_airplay client/airplay_win.cpp
        "${GST_ROOT}/include/openssl/applink.c")
    target_include_directories(vkr_airplay PRIVATE ${UX} "${GST_ROOT}/include")
    target_link_libraries(vkr_airplay PRIVATE
        vkr_raop_win vkrender_client GStreamer::win d3d11 dxgi)
    target_compile_options(vkr_airplay PRIVATE /EHsc)
endif()

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
