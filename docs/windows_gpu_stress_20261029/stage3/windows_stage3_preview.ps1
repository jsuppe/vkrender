$r='C:/Users/jonsu/dev/vkrender-control-20261029/build-control'
$env:PATH="$r/Release;C:/Users/jonsu/dev/framelink-nv12-20261029/build-trace/Release;"+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
python C:/Users/jonsu/windows_stage3_preview.py $r
exit $LASTEXITCODE
