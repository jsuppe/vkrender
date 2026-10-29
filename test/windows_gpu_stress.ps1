param([string]$RendererBuild='C:\Users\jonsu\dev\vkrender-nv12-20261029\build-trace', [string]$FrameLinkBuild='C:\Users\jonsu\dev\framelink-nv12-20261029\build-trace', [int]$Frames=18000)
$ErrorActionPreference='Stop'
$r=$RendererBuild;$f=$FrameLinkBuild
$env:PATH="$r\Release;$f\Release;"+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
$env:GST_PLUGIN_PATH="$f\Release"
$channel="gpu-stress-$PID"
$log=Join-Path $r $channel
New-Item -ItemType Directory -Force $log|Out-Null
$env:GST_REGISTRY="$log\registry.bin"
# No trace/readback in sustained interval; collect OS memory and consumer metadata.
$env:VKR_TRACE='';$env:FRAMELINK_TRACE='';$env:FRAMELINK_GST_TRACE=''
$all=[Collections.Generic.List[object]]::new();$memory=[Collections.Generic.List[object]]::new()
function Launch($exe,$arguments,$name){
 $p=Start-Process $exe -ArgumentList $arguments -WorkingDirectory $log -RedirectStandardOutput "$log\$name.out" -RedirectStandardError "$log\$name.err" -PassThru -NoNewWindow
 $null=$p.Handle;$all.Add($p);return $p
}
function Renderer($name){Launch "$r\Release\vkrenderer.exe" "--serve $channel --headless -1 --control-port 0 --scene-output $channel.rgb --scene-output $channel.nv12" $name}
function InputProducer($name){Launch ($env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin\gst-launch-1.0.exe') "-q videotestsrc is-live=true pattern=smpte ! video/x-raw,format=BGRA,width=1280,height=720,framerate=30/1 ! framelinksink channel=$channel.in.0 sync=false" $name}
function Sample($phase){
 $render.Refresh();$memory.Add([pscustomobject]@{utc=[DateTime]::UtcNow.ToString('o');phase=$phase;pid=$render.Id;private_bytes=$render.PrivateMemorySize64;working_set=$render.WorkingSet64;handles=$render.HandleCount})
}
function Capture($name,$size,$count,$interrupt='none'){
 $left=Launch "$f\Release\framelink_consumer.exe" "--channel $channel.rgb --format bgra --size $size --frames $count --verify 0 --map-cpu 0 --gpu-sync 1 --deadline-ms 30000" "$name-rgb"
 $right=Launch "$f\Release\framelink_consumer.exe" "--channel $channel.nv12 --format nv12 --size $size --frames $count --verify 0 --map-cpu 0 --gpu-sync 1 --deadline-ms 30000" "$name-nv12"
 $sw=[Diagnostics.Stopwatch]::StartNew();$done=$false
 while(!$left.HasExited -or !$right.HasExited){
  if($sw.Elapsed.TotalSeconds -gt 600){throw "capture timeout $name"}
  if($render.HasExited){throw "renderer exited $name"}
  if(!$done -and $sw.Elapsed.TotalSeconds -gt 3 -and $interrupt -eq 'input'){
   Stop-Process -Id $script:producer.Id -Force;Start-Sleep -Seconds 2;$script:producer=InputProducer "$name-input-restart";$done=$true
  }
  if(!$done -and $sw.Elapsed.TotalSeconds -gt 3 -and $interrupt -eq 'renderer'){
   Stop-Process -Id $script:producer.Id,$script:render.Id -Force
   Start-Sleep -Seconds 2
   $script:render=Renderer "$name-renderer-restart"
   $script:producer=InputProducer "$name-input-restart"
   $done=$true
  }
  Sample $name;Start-Sleep -Seconds 1
 }
 if($left.ExitCode -ne 0 -or $right.ExitCode -ne 0){throw "consumer failure $name : $($left.ExitCode),$($right.ExitCode)"}
 Get-Content "$log\$name-rgb.out","$log\$name-nv12.out"
}
try {
 $render=Renderer 'renderer';$script:producer=InputProducer 'input'
 Capture 'sustained1080' '1920x1080' $Frames
 # Same renderer reconnects to new rings/geometries, then recovers input process death.
 Capture 'resize720' '1280x720' 300 'input'
 Capture 'resize1080' '1920x1080' 300
 Capture 'surviving-consumers' '1920x1080' 600 'renderer'
 # Kill consumers with slots outstanding, then recreate their channel names.
 $a=Launch "$f\Release\framelink_consumer.exe" "--channel $channel.rgb --format bgra --size 640x360 --frames 999999 --verify 0 --gpu-sync 1 --delay-ms 30" 'abrupt-rgb'
 $b=Launch "$f\Release\framelink_consumer.exe" "--channel $channel.nv12 --format nv12 --size 640x360 --frames 999999 --verify 0 --gpu-sync 1 --delay-ms 30" 'abrupt-nv12'
 Start-Sleep -Seconds 3;Stop-Process -Id $a.Id,$b.Id -Force
 Capture 'after-consumer-crash' '1920x1080' 300
 Stop-Process -Id $script:producer.Id,$render.Id -Force
 $render=Renderer 'renderer-restarted';$script:producer=InputProducer 'input-restarted'
 Capture 'after-renderer-restart' '1920x1080' 300
 'PASS Windows GPU stress'|Tee-Object "$log\result.txt"
} finally {
 $memory|Export-Csv "$log\memory.csv" -NoTypeInformation
 foreach($p in $all){if(!$p.HasExited){Stop-Process -Id $p.Id -Force}}
 Write-Output "EVIDENCE=$log"
}
