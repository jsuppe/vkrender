
if(NOT "C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp/volk-populate-gitinfo.txt" IS_NEWER_THAN "C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp/volk-populate-gitclone-lastrun.txt")
  message(STATUS "Avoiding repeated git clone, stamp file is up to date: 'C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp/volk-populate-gitclone-lastrun.txt'")
  return()
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E rm -rf "C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-src"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to remove directory: 'C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-src'")
endif()

# try the clone 3 times in case there is an odd git clone issue
set(error_code 1)
set(number_of_tries 0)
while(error_code AND number_of_tries LESS 3)
  execute_process(
    COMMAND "C:/Program Files/Git/cmd/git.exe"  clone --no-checkout --config "advice.detachedHead=false" "https://github.com/zeux/volk.git" "volk-src"
    WORKING_DIRECTORY "C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps"
    RESULT_VARIABLE error_code
    )
  math(EXPR number_of_tries "${number_of_tries} + 1")
endwhile()
if(number_of_tries GREATER 1)
  message(STATUS "Had to git clone more than once:
          ${number_of_tries} times.")
endif()
if(error_code)
  message(FATAL_ERROR "Failed to clone repository: 'https://github.com/zeux/volk.git'")
endif()

execute_process(
  COMMAND "C:/Program Files/Git/cmd/git.exe"  checkout vulkan-sdk-1.3.296.0 --
  WORKING_DIRECTORY "C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-src"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to checkout tag: 'vulkan-sdk-1.3.296.0'")
endif()

set(init_submodules TRUE)
if(init_submodules)
  execute_process(
    COMMAND "C:/Program Files/Git/cmd/git.exe"  submodule update --recursive --init 
    WORKING_DIRECTORY "C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-src"
    RESULT_VARIABLE error_code
    )
endif()
if(error_code)
  message(FATAL_ERROR "Failed to update submodules in: 'C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-src'")
endif()

# Complete success, update the script-last-run stamp file:
#
execute_process(
  COMMAND ${CMAKE_COMMAND} -E copy
    "C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp/volk-populate-gitinfo.txt"
    "C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp/volk-populate-gitclone-lastrun.txt"
  RESULT_VARIABLE error_code
  )
if(error_code)
  message(FATAL_ERROR "Failed to copy script-last-run stamp file: 'C:/Users/jonsu/dev/vkrender/android/app/.cxx/Debug/52723z1s/arm64-v8a/_deps/volk-subbuild/volk-populate-prefix/src/volk-populate-stamp/volk-populate-gitclone-lastrun.txt'")
endif()

