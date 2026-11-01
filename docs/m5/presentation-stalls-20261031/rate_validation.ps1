$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final"
$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$env:VKR_TRACE_EVENTS='262144';$env:FRAMELINK_TRACE_EVENTS='262144';$env:FRAMELINK_GST_TRACE_EVENTS='262144'
foreach($fmt in @('BGRA','NV12')){
 python "$d/m5_native_rate_probe.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --clock-policy producer --seconds 45 --case "1920x1080-60-$fmt-av" --output "$d/m5-power-fixed-native" *> "$d/m5-power-fixed-native-$fmt.log"
 @{case=$fmt;trace='on';exit=$LASTEXITCODE;finished=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress|Add-Content "$d/m5-power-fixed-rate-results.jsonl"
}
python "$d/m5_trace_overhead_probe.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --clock-policy producer --seconds 45 --case '1920x1080-60-BGRA-av' --trace off --output "$d/m5-power-fixed-trace-off" *> "$d/m5-power-fixed-trace-off.log"
@{case='BGRA';trace='off';exit=$LASTEXITCODE;finished=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress|Add-Content "$d/m5-power-fixed-rate-results.jsonl"
'complete'|Set-Content "$d/m5-power-fixed-rate.done"
