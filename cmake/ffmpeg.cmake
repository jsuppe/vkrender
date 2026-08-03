# Prebuilt FFmpeg (BtbN win64 shared build: MSVC-compatible .lib import
# libraries + DLLs). Downloaded at configure time unless VKR_FFMPEG_ROOT
# points at an existing extracted build.

set(VKR_FFMPEG_PKG "ffmpeg-n7.1-latest-win64-gpl-shared-7.1")
set(VKR_FFMPEG_URL
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/${VKR_FFMPEG_PKG}.zip")

if(NOT DEFINED VKR_FFMPEG_ROOT)
    set(_ff_base "${CMAKE_BINARY_DIR}/ffmpeg-prebuilt")
    set(VKR_FFMPEG_ROOT "${_ff_base}/${VKR_FFMPEG_PKG}")
    if(NOT EXISTS "${VKR_FFMPEG_ROOT}/lib/avcodec.lib")
        set(_ff_zip "${_ff_base}/${VKR_FFMPEG_PKG}.zip")
        if(NOT EXISTS "${_ff_zip}")
            message(STATUS "Downloading FFmpeg prebuilt: ${VKR_FFMPEG_URL}")
            file(DOWNLOAD "${VKR_FFMPEG_URL}" "${_ff_zip}" SHOW_PROGRESS STATUS _dl_status)
            list(GET _dl_status 0 _dl_code)
            if(NOT _dl_code EQUAL 0)
                file(REMOVE "${_ff_zip}")
                message(FATAL_ERROR "FFmpeg download failed: ${_dl_status}")
            endif()
        endif()
        message(STATUS "Extracting FFmpeg prebuilt")
        file(ARCHIVE_EXTRACT INPUT "${_ff_zip}" DESTINATION "${_ff_base}")
    endif()
endif()

if(NOT EXISTS "${VKR_FFMPEG_ROOT}/lib/avcodec.lib")
    message(FATAL_ERROR "FFmpeg not found at ${VKR_FFMPEG_ROOT}")
endif()

foreach(_lib avutil avcodec avformat)
    file(GLOB _dll "${VKR_FFMPEG_ROOT}/bin/${_lib}-*.dll")
    add_library(FFmpeg::${_lib} SHARED IMPORTED)
    set_target_properties(FFmpeg::${_lib} PROPERTIES
        IMPORTED_IMPLIB "${VKR_FFMPEG_ROOT}/lib/${_lib}.lib"
        IMPORTED_LOCATION "${_dll}"
        INTERFACE_INCLUDE_DIRECTORIES "${VKR_FFMPEG_ROOT}/include")
endforeach()

# FFmpeg DLLs have interdependencies (avcodec -> swresample, ...); copy the
# whole bin/ set next to the target.
function(vkr_copy_ffmpeg_dlls target)
    file(GLOB _dlls "${VKR_FFMPEG_ROOT}/bin/*.dll")
    foreach(_dll ${_dlls})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dll}" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
endfunction()
