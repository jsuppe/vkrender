# Two independent FrameLink consumers. Close the video window (or Ctrl+C) to stop.
param(
 [Parameter(Mandatory=$true)][string]$RendererBuild,
 [Parameter(Mandatory=$true)][string]$FrameLinkBuild,
 [switch]$Verify
)
$ErrorActionPreference='Stop'
$RendererBuild=(Resolve-Path $RendererBuild).Path
$fl=Join-Path (Resolve-Path $FrameLinkBuild).Path 'Release'
$env:PATH=$fl+';'+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
$env:GST_PLUGIN_PATH=$fl
$env:GST_REGISTRY=Join-Path $RendererBuild 'compare-registry.bin'
$gst=$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin\gst-launch-1.0.exe'
$channel="framelink-compare-$PID"
$log=Join-Path $RendererBuild $channel
New-Item -ItemType Directory -Force $log | Out-Null
$renderer=$null;$producer=$null;$consumer=$null
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
 $renderer=Start-Process "$RendererBuild\Release\vkrenderer.exe" -NoNewWindow -ArgumentList "--serve $channel --headless -1 --control-port 0 --scene-output $channel.rgb --scene-output $channel.nv12" -WorkingDirectory $log -PassThru -RedirectStandardOutput "$log\renderer.out" -RedirectStandardError "$log\renderer.err"
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
 } else {
  Write-Output "Running side-by-side comparison. Logs: $log"
  $consumer.WaitForExit()
 }
} finally {
 foreach($p in @($producer,$renderer,$consumer)){if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}}
}
