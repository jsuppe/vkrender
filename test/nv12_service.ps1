param(
 [Parameter(Mandatory=$true)][string]$RendererBuild,
 [Parameter(Mandatory=$true)][string]$FrameLinkBuild,
 [ValidateSet('NV12','BGRA')][string]$Format='NV12'
)
$ErrorActionPreference='Stop'
$RendererBuild=(Resolve-Path $RendererBuild).Path
$root=Split-Path $RendererBuild -Parent
$channel="nv12-service-test-$PID"
$stride=if($Format -eq 'NV12'){132}else{520}
$frameBytes=if($Format -eq 'NV12'){132*66*3/2}else{520*66}
$fl=Join-Path (Resolve-Path $FrameLinkBuild).Path 'Release'
$env:PATH=$fl+';'+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
$env:GST_PLUGIN_PATH=$fl
$env:GST_REGISTRY=$root+'\build\gst-test-registry.bin'
$env:VKR_NO_DISPLAY_WAKE='1'
$gst=$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin\gst-launch-1.0.exe'
$receiver=$null;$renderer=$null;$producer=$null
try {
 $receiver=Start-Process $gst -ArgumentList "-q framelinksrc channel=$channel.scene format=$Format width=130 height=66 num-buffers=30 ! filesink location=nv12-service.raw" -WorkingDirectory $root -PassThru -RedirectStandardOutput "$root\build\nv12-receive.out" -RedirectStandardError "$root\build\nv12-receive.err"
 $null=$receiver.Handle
 $renderer=Start-Process "$root\build\Release\vkrenderer.exe" -ArgumentList "--serve $channel --headless -1 --control-port 0" -WorkingDirectory $root -PassThru -RedirectStandardOutput "$root\build\nv12-render.out" -RedirectStandardError "$root\build\nv12-render.err"
 Start-Sleep -Seconds 2
 $producer=Start-Process "$fl\fl_produce.exe" -ArgumentList "$channel.in.0 300 16" -WorkingDirectory $root -PassThru -RedirectStandardOutput "$root\build\nv12-produce.out" -RedirectStandardError "$root\build\nv12-produce.err"
 if(!$receiver.WaitForExit(20000)){throw 'Receiver timeout'}
 if($receiver.ExitCode -ne 0){throw "Receiver exit $($receiver.ExitCode)"}
 $bytes=[IO.File]::ReadAllBytes("$root\nv12-service.raw")
 if($bytes.Length -ne $frameBytes*30){throw "Wrong byte count $($bytes.Length)"}
 $hashes=[Collections.Generic.HashSet[string]]::new();$nonNeutral=0
 for($i=0;$i -lt 30;$i++){
  $frame=$bytes[($i*$frameBytes)..(($i+1)*$frameBytes-1)]
  $hashes.Add([Convert]::ToBase64String([Security.Cryptography.SHA256]::Create().ComputeHash($frame)))|Out-Null
  for($j=($stride*66);$j -lt $frameBytes;$j++){if($frame[$j] -gt 16 -and $frame[$j] -lt 240 -and $frame[$j] -ne 128){$nonNeutral++;break}}
 }
 if($hashes.Count -lt 2 -or ($Format -eq 'NV12' -and $nonNeutral -lt 1)){throw 'No changing/colorful NV12 frames'}
 Write-Output "PASS: 30 renderer service $Format frames, $($hashes.Count) distinct, $nonNeutral with non-neutral chroma, $($bytes.Length) bytes"
} finally {
 foreach($p in @($producer,$renderer,$receiver)){if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}}
}
