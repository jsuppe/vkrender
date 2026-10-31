$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final";$b="$r/framelink/Release";$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$env:VKR_TRACE_EVENTS='262144';$env:FRAMELINK_TRACE_EVENTS='262144';$env:FRAMELINK_GST_TRACE_EVENTS='262144'
$env:VKR_TRACE_RETIRE='1';$env:FRAMELINK_TRACE_RING='1'
$cases=@(
 @{slots=8;phase='control';case='1920x1080-60-BGRA-av'},
 @{slots=12;phase='first';case='1920x1080-60-BGRA-av'},
 @{slots=8;phase='control';case='1920x1080-60-NV12-av'},
 @{slots=12;phase='first';case='1920x1080-60-NV12-av'},
 @{slots=12;phase='first';case='1920x1080-30-BGRA-av'},
 @{slots=12;phase='first';case='1920x1080-30-NV12-av'},
 @{slots=12;phase='repeat';case='1920x1080-60-BGRA-av'},
 @{slots=12;phase='repeat';case='1920x1080-60-NV12-av'}
)
foreach($c in $cases) {
 $env:VKR_TEST_AV_POOL_SLOTS=[string]$c.slots
 $name="m5-capacity-$($c.slots)-$($c.phase)";$case=$c.case
 python "$d/m5_native_rate_probe.py" --build $b --renderer "$r/Release/vkrenderer.exe" --clock-policy producer --seconds 45 --case $case --output "$d/$name" *> "$d/$name-$case.log"
 @{slots=$c.slots;phase=$c.phase;case=$case;exit=$LASTEXITCODE;finished=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Compress|Add-Content "$d/m5-capacity-matrix-progress.jsonl"
}
