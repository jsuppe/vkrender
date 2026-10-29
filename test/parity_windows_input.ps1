param([Parameter(Mandatory=$true)][string]$RendererBuild, [Parameter(Mandatory=$true)][string]$FrameLinkBuild)
$ErrorActionPreference='Stop'
$r=(Resolve-Path $RendererBuild).Path
$f=(Resolve-Path $FrameLinkBuild).Path
$env:PATH="$r\Release;$f\Release;"+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
$env:GST_PLUGIN_PATH="$f\Release"
$env:VKR_INPUT_FORMATS='nv12'
$env:VKR_LAYOUT='wall'
$render=$null;$producer=$null
try {
 $render=Start-Process "$r\Release\vkrenderer.exe" -ArgumentList '--serve parity-nvinput --headless 40 --window 640x360+0+0 --dump parity-nvinput.ppm' -WorkingDirectory $r -RedirectStandardOutput "$r\parity-nvinput.out" -RedirectStandardError "$r\parity-nvinput.err" -PassThru -NoNewWindow
 $null=$render.Handle
 Start-Sleep -Milliseconds 500
 $producer=Start-Process gst-launch-1.0.exe -ArgumentList '-q videotestsrc pattern=red is-live=true num-buffers=150 ! video/x-raw,format=NV12,width=640,height=360,framerate=30/1,colorimetry=bt709 ! framelinksink channel=parity-nvinput.in.0 sync=false' -RedirectStandardOutput "$r\parity-nvinput-producer.out" -RedirectStandardError "$r\parity-nvinput-producer.err" -PassThru -NoNewWindow
 if(!$render.WaitForExit(20000)){throw 'renderer timeout'}
 if($render.ExitCode -ne 0){throw "renderer exit $($render.ExitCode)"}
 Write-Output 'PASS: Windows NV12 input rendered 40 frames'
} finally {
 foreach($p in @($producer,$render)){if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}}
}
