$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final";$n='m5-power-fixed-gates'
$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$env:VKR_TRACE_EVENTS='262144';$env:FRAMELINK_TRACE_EVENTS='262144';$env:FRAMELINK_GST_TRACE_EVENTS='262144'
$env:VK_LAYER_PATH="$d/vulkan-validation-local/Bin";$env:VKR_VALIDATION='1'
Get-FileHash "$r/Release/vkrenderer.exe","$r/framelink/Release/framelink.dll" | ConvertTo-Json | Set-Content "$d/$n-hashes.json"
foreach($case in @('prepared','late-power-negative','lazy-negative','failure','timeout')){
 $env:VKR_TEST_AV_LATE_POWER_REQUEST=$null
 $testcase=$case
 if($case -eq 'late-power-negative'){$env:VKR_TEST_AV_LATE_POWER_REQUEST='1';$testcase='prepared'}
 python "$d/native_av_preparation.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output "$d/$n/$case" --case $testcase --require-early-power *> "$d/$n-$case.log"
 $code=$LASTEXITCODE;$ok=$code -eq 0
 if($case -eq 'late-power-negative'){$ok=($code -ne 0) -and [bool](Select-String -Path "$d/$n-$case.log" -Pattern 'AssertionError: power request followed timed source' -Quiet)}
 @{case=$case;exit=$code;expected_result=$ok}|ConvertTo-Json -Compress|Add-Content "$d/$n-results.jsonl"
}
$env:VKR_TEST_AV_LATE_POWER_REQUEST=$null
'complete'|Set-Content "$d/$n.done"
