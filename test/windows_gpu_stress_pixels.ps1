param([Parameter(Mandatory=$true)][string]$RendererBuild, [Parameter(Mandatory=$true)][string]$FrameLinkBuild, [switch]$Rgb10, [int]$Frames=6000, [int]$TimeoutSeconds=240)
$ErrorActionPreference='Stop'
$r=(Resolve-Path $RendererBuild).Path
$f=(Resolve-Path $FrameLinkBuild).Path
$env:PATH="$r\Release;$f\Release;"+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
$env:GST_PLUGIN_PATH="$f\Release"
$env:VKR_CHECK_FRAMES="$Frames";$env:VKR_CHECK_TIMEOUT="$TimeoutSeconds"
$check=$null;$render=$null;$producer=$null
try {
 $check=Start-Process "$r\Release\vkr_portable_output_check.exe" -ArgumentList $(if($Rgb10){"windows-stress-pixels rgb10a2"}else{"windows-stress-pixels"}) -RedirectStandardOutput "$r\parity-output-check.out" -RedirectStandardError "$r\parity-output-check.err" -PassThru -NoNewWindow
 $null=$check.Handle
 $render=Start-Process "$r\Release\vkrenderer.exe" -ArgumentList '--serve windows-stress-pixels --scene-output windows-stress-pixels.rgba --scene-output windows-stress-pixels.nv12 --headless -1 --window 640x360+0+0' -WorkingDirectory $r -RedirectStandardOutput "$r\parity-output-render.out" -RedirectStandardError "$r\parity-output-render.err" -PassThru -NoNewWindow
 $null=$render.Handle
 Start-Sleep -Milliseconds 500
 $producer=Start-Process gst-launch-1.0.exe -ArgumentList '-q videotestsrc pattern=smpte is-live=true num-buffers=15000 ! video/x-raw,format=BGRA,width=640,height=360,framerate=30/1 ! framelinksink channel=windows-stress-pixels.in.0 sync=false' -RedirectStandardOutput "$r\parity-output-producer.out" -RedirectStandardError "$r\parity-output-producer.err" -PassThru -NoNewWindow
 if(!$check.WaitForExit(($TimeoutSeconds+10)*1000)){throw 'checker timeout'}
 Get-Content "$r\parity-output-check.out"
 if($check.ExitCode -ne 0){throw "checker exit $($check.ExitCode)"}
} finally {
 foreach($p in @($producer,$render,$check)){if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}}
}
