$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final";$b="$r/framelink/Release";$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$env:VKR_TRACE_EVENTS='262144';$env:FRAMELINK_TRACE_EVENTS='262144';$env:FRAMELINK_GST_TRACE_EVENTS='262144'
$env:VK_LAYER_PATH="$d/vulkan-validation-local/Bin"
foreach($case in @('1920x1080-30-BGRA-av','1920x1080-30-NV12-av','1920x1080-60-BGRA-av','1920x1080-60-NV12-av')) {
 python "$d/m5_native_rate_probe.py" --build $b --renderer "$r/Release/vkrenderer.exe" --clock-policy producer --seconds 45 --case $case --output "$d/m5-native-accounted" *> "$d/m5-native-accounted-$case.log"
}
'complete' | Set-Content "$d/m5-native-accounted.done"
