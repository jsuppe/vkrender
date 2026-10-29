# Two independent FrameLink consumers. Close the video window (or Ctrl+C) to stop.
param(
 [Parameter(Mandatory=$true)][string]$RendererBuild,
 [Parameter(Mandatory=$true)][string]$FrameLinkBuild,
 [switch]$Verify,
 [switch]$CaptureTrace
)
$ErrorActionPreference='Stop'
if($CaptureTrace){$Verify=$true}
$traceKeys=@('VKR_TRACE','VKR_TRACE_GPU','FRAMELINK_TRACE','FRAMELINK_GST_TRACE')
$savedTrace=@{}
foreach($key in $traceKeys){$savedTrace[$key]=[Environment]::GetEnvironmentVariable($key)}
$RendererBuild=(Resolve-Path $RendererBuild).Path
$fl=Join-Path (Resolve-Path $FrameLinkBuild).Path 'Release'
$env:PATH=$fl+';'+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
$env:GST_PLUGIN_PATH=$fl
$env:GST_REGISTRY=Join-Path $RendererBuild 'compare-registry.bin'
$gst=$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin\gst-launch-1.0.exe'
$channel="framelink-compare-$PID-"+[guid]::NewGuid().ToString('N').Substring(0,8)
$log=Join-Path $RendererBuild $channel
New-Item -ItemType Directory -Force $log | Out-Null
$renderer=$null;$producer=$null;$consumer=$null
if($CaptureTrace){
 $env:VKR_TRACE="$log\renderer"
 $env:VKR_TRACE_GPU='1'
 $env:FRAMELINK_TRACE="$log\transport"
 $env:FRAMELINK_GST_TRACE="$log\gstreamer"
}
$headless=if($CaptureTrace){90}else{-1}
try {
 $limit=if($Verify){'num-buffers=60'}else{''}
 $sink=if($Verify){'fakesink sync=false'}else{'d3d11videosink sync=false'}
 $leftCapture=if($Verify){'tee name=l l. ! queue ! filesink location=rgb.raw l. ! queue !'}else{''}
 $rightCapture=if($Verify){'tee name=r r. ! queue ! filesink location=nv12-rgb.raw r. ! queue !'}else{''}
 # GStreamer sources own different channels and negotiate different texture formats.
 # Explicit BT.709 limited-range metadata describes the renderer's NV12 output.
 $pipeline="-e compositor name=mix sink_0::xpos=0 sink_1::xpos=640 ! video/x-raw,width=1280,height=360 ! videoconvert ! $sink framelinksrc channel=$channel.rgb format=BGRA width=640 height=360 fps=30 $limit ! $leftCapture textoverlay text=RGB-via-FrameLink valignment=top shaded-background=true ! mix.sink_0 framelinksrc channel=$channel.nv12 format=NV12 width=640 height=360 fps=30 $limit ! video/x-raw,colorimetry=bt709,chroma-site=mpeg2 ! videoconvert ! video/x-raw,format=BGRA ! $rightCapture textoverlay text=NV12-via-FrameLink-to-RGB valignment=top shaded-background=true ! mix.sink_1"
 $consumer=Start-Process $gst -NoNewWindow -ArgumentList $pipeline -WorkingDirectory $log -PassThru -RedirectStandardOutput "$log\consumer.out" -RedirectStandardError "$log\consumer.err"
 $null=$consumer.Handle
 $renderer=Start-Process "$RendererBuild\Release\vkrenderer.exe" -NoNewWindow -ArgumentList "--serve $channel --headless $headless --control-port 0 --scene-output $channel.rgb --scene-output $channel.nv12" -WorkingDirectory $log -PassThru -RedirectStandardOutput "$log\renderer.out" -RedirectStandardError "$log\renderer.err"
 $null=$renderer.Handle
 Start-Sleep -Seconds 2
 # Timestamp and changing SMPTE noise make freeze/repeat errors visible.
 $producer=Start-Process $gst -NoNewWindow -ArgumentList "-e videotestsrc is-live=true pattern=smpte ! video/x-raw,format=BGRA,width=640,height=360,framerate=30/1 ! timeoverlay valignment=bottom ! framelinksink channel=$channel.in.0" -WorkingDirectory $log -PassThru -RedirectStandardOutput "$log\producer.out" -RedirectStandardError "$log\producer.err"
 @{Renderer=$renderer.Id;Producer=$producer.Id;Consumer=$consumer.Id;Channel=$channel;Logs=$log}|ConvertTo-Json|Set-Content "$RendererBuild\compare-processes.json"
 if($Verify){
  if(!$consumer.WaitForExit(30000)){throw "Consumer timeout; inspect $log"}
  if($consumer.ExitCode -ne 0){throw "Consumer failed; inspect $log"}
  foreach($name in @('rgb','nv12-rgb')){
   $bytes=[IO.File]::ReadAllBytes("$log\$name.raw")
   $size=640*360*4
   if($bytes.Length -ne 60*$size){throw "Wrong $name frame count: $($bytes.Length) bytes"}
   $hashes=[Collections.Generic.HashSet[string]]::new()
   $sha=[Security.Cryptography.SHA256]::Create()
   for($i=0;$i -lt 60;$i++){[void]$hashes.Add([Convert]::ToBase64String($sha.ComputeHash($bytes,$i*$size,$size)))}
   $sha.Dispose()
   if($hashes.Count -lt 30){throw "Insufficient changing frames in $name : $($hashes.Count)"}
   Write-Output "PASS $name : 60 received frames, $($hashes.Count) distinct"
  }
  if($CaptureTrace){
   if(!$renderer.WaitForExit(20000)){throw 'Trace renderer did not exit cleanly'}
   if($renderer.ExitCode -ne 0){throw 'Trace renderer failed'}
   $files=Get-ChildItem "$log\renderer.*.csv"
   if(!$files){throw 'Renderer trace missing'}
   $text=Get-Content $files[0].FullName
   if(!($text -match ',scene.queue_submit_cpu,')){throw 'Submission trace missing'}
   if(!($text -match ',gpu.scene_duration,')){throw 'GPU duration missing (requires timestamp-capable queue)'}
   if(!($text -match '# dropped_events=0,rejected_threads=0,')){throw 'Trace truncated'}
   Write-Output "PASS renderer CPU/GPU trace: $($files[0].FullName)"
  }
 } else {
  Write-Output "Running side-by-side comparison. Logs: $log"
  $consumer.WaitForExit()
 }
} finally {
 foreach($p in @($producer,$renderer,$consumer)){if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}}
 if($CaptureTrace){foreach($key in $traceKeys){[Environment]::SetEnvironmentVariable($key,$savedTrace[$key])}}
}
