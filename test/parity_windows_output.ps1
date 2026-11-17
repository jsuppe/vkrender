# Scene output end to end: the renderer publishes its scene to an RGB and an
# NV12 channel created by vkr_portable_output_check, which compares 20
# timestamp-matched pairs. Needs a GPU and the framelink GStreamer plugin.
# BinDir holds vkrenderer.exe, vkr_portable_output_check.exe and framelink.dll;
# PluginDir holds gstframelink.dll. Logs go to LogDir (default: BinDir).
param([Parameter(Mandatory=$true)][string]$BinDir, [Parameter(Mandatory=$true)][string]$PluginDir,
      [string]$LogDir, [switch]$Rgb10)
$ErrorActionPreference='Stop'
$r=(Resolve-Path $BinDir).Path
$plugins=(Resolve-Path $PluginDir).Path
$log=if($LogDir){New-Item -ItemType Directory -Force $LogDir | Out-Null; (Resolve-Path $LogDir).Path}else{$r}
$gstRoot=if($env:GSTREAMER_1_0_ROOT_MSVC_X86_64){$env:GSTREAMER_1_0_ROOT_MSVC_X86_64}else{'C:\Program Files\gstreamer\1.0\msvc_x86_64\'}
$env:PATH="$r;"+(Join-Path $gstRoot 'bin')+";"+$env:PATH
$env:GST_PLUGIN_PATH=$plugins
$env:GST_REGISTRY=Join-Path $log 'parity-output-registry.bin'
$prefix='parity-native-'+$PID
$check=$null;$render=$null;$producer=$null
try {
 $check=Start-Process "$r\vkr_portable_output_check.exe" -ArgumentList $(if($Rgb10){"$prefix rgb10a2"}else{$prefix}) -RedirectStandardOutput "$log\parity-output-check.out" -RedirectStandardError "$log\parity-output-check.err" -PassThru -NoNewWindow
 $null=$check.Handle
 $render=Start-Process "$r\vkrenderer.exe" -ArgumentList "--serve $prefix --scene-output $prefix.rgba --scene-output $prefix.nv12 --headless 300 --window 640x360+0+0 --dump parity-native.ppm" -WorkingDirectory $log -RedirectStandardOutput "$log\parity-output-render.out" -RedirectStandardError "$log\parity-output-render.err" -PassThru -NoNewWindow
 $null=$render.Handle
 Start-Sleep -Milliseconds 500
 $producer=Start-Process gst-launch-1.0.exe -ArgumentList "-q videotestsrc pattern=red is-live=true num-buffers=450 ! video/x-raw,format=BGRA,width=640,height=360,framerate=30/1 ! framelinksink channel=$prefix.in.0 sync=false" -RedirectStandardOutput "$log\parity-output-producer.out" -RedirectStandardError "$log\parity-output-producer.err" -PassThru -NoNewWindow
 if(!$check.WaitForExit(20000)){throw 'checker timeout'}
 Get-Content "$log\parity-output-check.out"
 if($check.ExitCode -ne 0){Get-Content "$log\parity-output-render.err" -ErrorAction SilentlyContinue | Select-Object -Last 20; throw "checker exit $($check.ExitCode)"}
} finally {
 foreach($p in @($producer,$render,$check)){if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}}
}
