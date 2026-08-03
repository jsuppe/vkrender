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

# --- FFmpeg ------------------------------------------------------------------
if(WIN32)
    include(${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake) # prebuilt BtbN download
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavformat>=60 libavcodec>=60 libavutil>=58)
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
    target_sources(vkdecoder PRIVATE decoder/d3d11va_decoder.cpp)
    target_link_libraries(vkdecoder PRIVATE
        FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil d3d11 dxgi)
    vkr_copy_ffmpeg_dlls(vkdecoder)
else()
    target_sources(vkdecoder PRIVATE decoder/vaapi_decoder.cpp)
    target_link_libraries(vkdecoder PRIVATE PkgConfig::FFMPEG PkgConfig::LIBVA)
endif()

# --- shader compilation ------------------------------------------------------
set(VKR_SHADERS
    ${CMAKE_SOURCE_DIR}/renderer/shaders/fullscreen.vert
    ${CMAKE_SOURCE_DIR}/renderer/shaders/video_ycbcr.frag)
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
    renderer/video_pass.cpp)
target_link_libraries(vkrenderer PRIVATE vkr_common volk glfw)
add_dependencies(vkrenderer vkr_shaders)
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
