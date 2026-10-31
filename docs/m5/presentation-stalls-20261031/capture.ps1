$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final"
$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$env:VKR_TRACE_EVENTS='262144';$env:FRAMELINK_TRACE_EVENTS='262144';$env:FRAMELINK_GST_TRACE_EVENTS='262144'
try {
 Get-FileHash "$r/Release/vkrenderer.exe","$r/framelink/Release/framelink.dll" | ConvertTo-Json | Set-Content "$d/m5-present-baseline-hashes.json"
 python "$d/native_av_preparation.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output "$d/m5-present-etw-baseline" --case prepared *> "$d/m5-present-etw-baseline.log"
 @{exit=$LASTEXITCODE;finished=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$d/m5-present-etw-baseline.done"
} catch { $_ | Out-File "$d/m5-present-etw-baseline.error" }
