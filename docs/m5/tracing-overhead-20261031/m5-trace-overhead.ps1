$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final";$b="$r/framelink/Release";$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$env:VKR_TRACE_EVENTS='262144';$env:FRAMELINK_TRACE_EVENTS='262144';$env:FRAMELINK_GST_TRACE_EVENTS='262144';$env:VKR_TEST_AV_POOL_SLOTS='12'
foreach($fmt in @('BGRA','NV12')) {foreach($trace in @('off','on')) {
 $case="1920x1080-60-$fmt-av";$name="m5-trace-$trace"
 python "$d/m5_trace_overhead_probe.py" --build $b --renderer "$r/Release/vkrenderer.exe" --clock-policy producer --seconds 45 --case $case --trace $trace --output "$d/$name" *> "$d/$name-$case.log"
 @{trace=$trace;case=$case;exit=$LASTEXITCODE;finished=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress|Add-Content "$d/m5-trace-overhead-progress.jsonl"
}}
