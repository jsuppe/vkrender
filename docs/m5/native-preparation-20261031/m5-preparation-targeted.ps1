$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final";$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
foreach($case in @('lazy-negative','failure','timeout')){
 python "$d/native_av_preparation.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output "$d/m5-native-preparation-negatives" --case $case *> "$d/m5-native-preparation-$case.log"
 @{case=$case;exit=$LASTEXITCODE}|ConvertTo-Json -Compress|Add-Content "$d/m5-native-preparation-negative-results.jsonl"
}
$env:VKR_TRACE_EVENTS='262144';$env:FRAMELINK_TRACE_EVENTS='262144';$env:FRAMELINK_GST_TRACE_EVENTS='262144'
python "$d/m5_native_rate_probe.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --clock-policy producer --seconds 45 --case '1920x1080-60-NV12-av' --output "$d/m5-prepared-normal" *> "$d/m5-prepared-normal.log"
@{normal=$LASTEXITCODE}|ConvertTo-Json|Set-Content "$d/m5-prepared-normal-result.json"
