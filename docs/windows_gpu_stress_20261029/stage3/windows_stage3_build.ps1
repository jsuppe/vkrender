$ErrorActionPreference='Stop'
$r='C:/Users/jonsu/dev/vkrender-control-20261029'
cmake -S $r -B "$r/build-control" -G 'Visual Studio 17 2022' -A x64 -DFRAMELINK_DIR=C:/Users/jonsu/dev/framelink-nv12-20261029 -DFETCHCONTENT_SOURCE_DIR_GLFW=C:/Users/jonsu/dev/vkrender/build/_deps/glfw-src -DFETCHCONTENT_SOURCE_DIR_GLSLANG=C:/Users/jonsu/dev/vkrender/build/_deps/glslang-src -DFETCHCONTENT_SOURCE_DIR_VOLK=C:/Users/jonsu/dev/vkrender/build/_deps/volk-src -DFETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS=C:/Users/jonsu/dev/vkrender/build/_deps/vulkan_headers-src
if($LASTEXITCODE){exit $LASTEXITCODE}
cmake --build "$r/build-control" --config Release --target vkrenderer vkr_control_test vkr_scene_layout_test vkr_nv12_output_test --parallel 4
if($LASTEXITCODE){exit $LASTEXITCODE}
$env:PATH="C:/Users/jonsu/dev/framelink-nv12-20261029/build-trace/Release;"+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
ctest --test-dir "$r/build-control" -C Release --output-on-failure -R 'shared_scene_layouts|shared_moderation|control_http|control_preview_admission|nv12_gpu'
exit $LASTEXITCODE
