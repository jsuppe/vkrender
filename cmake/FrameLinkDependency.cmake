include_guard(GLOBAL)

# One source pin for desktop and Android. FrameLink is built from source, not
# loaded from an arbitrary installed DLL or an old sibling include directory.
function(vkr_add_framelink renderer_root)
    file(READ "${renderer_root}/cmake/framelink-revision.txt" expected_revision)
    string(STRIP "${expected_revision}" expected_revision)
    set(FRAMELINK_DIR "${renderer_root}/../framelink" CACHE PATH
        "Path to the pinned FrameLink checkout (see cmake/framelink-revision.txt)")
    if(NOT EXISTS "${FRAMELINK_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "FrameLink source not found at '${FRAMELINK_DIR}'. Check out "
            "${expected_revision} and set -DFRAMELINK_DIR=<checkout> "
            "(Android Gradle: -PframelinkDir=<checkout>).")
    endif()
    find_package(Git REQUIRED)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FRAMELINK_DIR}" rev-parse HEAD
        RESULT_VARIABLE git_result OUTPUT_VARIABLE actual_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT git_result EQUAL 0 OR NOT actual_revision STREQUAL expected_revision)
        message(FATAL_ERROR
            "FrameLink revision mismatch at '${FRAMELINK_DIR}': expected "
            "${expected_revision}, found '${actual_revision}'. Use a dedicated "
            "checkout/worktree; do not reset another task's working tree.")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FRAMELINK_DIR}"
        diff --quiet HEAD -- CMakeLists.txt src include gst samples
        RESULT_VARIABLE source_dirty)
    if(NOT source_dirty EQUAL 0)
        message(FATAL_ERROR "Pinned FrameLink implementation has local edits; use a clean checkout.")
    endif()
    file(STRINGS "${FRAMELINK_DIR}/include/framelink.h" version_line
        REGEX "^#define FL_VERSION +[0-9]+")
    if(NOT version_line MATCHES "^#define FL_VERSION +6$")
        message(FATAL_ERROR "This renderer integration requires FrameLink protocol version 6.")
    endif()
    message(STATUS "vkrender FrameLink pin: ${actual_revision} (protocol 6)")
    set(FRAMELINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(ANDROID)
        # The native renderer has no GStreamer dependency unless the A/V input
        # is enabled (VKR_GST_ANDROID_ROOT, FrameLink decision 12): then
        # FrameLink's static GStreamer element is built for the A/V session.
        if(VKR_GST_ANDROID_ROOT)
            set(FRAMELINK_BUILD_GST ON CACHE BOOL "" FORCE)
            set(FRAMELINK_GST_ANDROID_ROOT "${VKR_GST_ANDROID_ROOT}" CACHE PATH "" FORCE)
        else()
            set(FRAMELINK_BUILD_GST OFF CACHE BOOL "" FORCE)
        endif()
    endif()
    if(NOT TARGET framelink)
        add_subdirectory("${FRAMELINK_DIR}" "${CMAKE_BINARY_DIR}/framelink")
    endif()
endfunction()
