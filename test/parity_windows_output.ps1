param([Parameter(Mandatory=$true)][string]$RendererBuild, [Parameter(Mandatory=$true)][string]$FrameLinkBuild, [switch]$Rgb10)
$ErrorActionPreference='Stop'
$r=(Resolve-Path $RendererBuild).Path
$f=(Resolve-Path $FrameLinkBuild).Path
$env:PATH="$r\Release;$f\Release;"+$env:GSTREAMER_1_0_ROOT_MSVC_X86_64+'bin;'+$env:PATH
$env:GST_PLUGIN_PATH="$f\Release"
$check=$null;$render=$null;$producer=$null
try {
 $check=Start-Process "$r\Release\vkr_portable_output_check.exe" -ArgumentList $(if($Rgb10){"parity-native rgb10a2"}else{"parity-native"}) -RedirectStandardOutput "$r\parity-output-check.out" -RedirectStandardError "$r\parity-output-check.err" -PassThru -NoNewWindow
 $null=$check.Handle
 $render=Start-Process "$r\Release\vkrenderer.exe" -ArgumentList '--serve parity-native --scene-output parity-native.rgba --scene-output parity-native.nv12 --headless 300 --window 640x360+0+0 --dump parity-native.ppm' -WorkingDirectory $r -RedirectStandardOutput "$r\parity-output-render.out" -RedirectStandardError "$r\parity-output-render.err" -PassThru -NoNewWindow
 $null=$render.Handle
 Start-Sleep -Milliseconds 500
 $producer=Start-Process gst-launch-1.0.exe -ArgumentList '-q videotestsrc pattern=red is-live=true num-buffers=450 ! video/x-raw,format=BGRA,width=640,height=360,framerate=30/1 ! framelinksink channel=parity-native.in.0 sync=false' -RedirectStandardOutput "$r\parity-output-producer.out" -RedirectStandardError "$r\parity-output-producer.err" -PassThru -NoNewWindow
 if(!$check.WaitForExit(20000)){throw 'checker timeout'}
 Get-Content "$r\parity-output-check.out"
 if($check.ExitCode -ne 0){throw "checker exit $($check.ExitCode)"}
} finally {
 foreach($p in @($producer,$render,$check)){if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}}
}
