<#
.SYNOPSIS
  Spawn N synthetic WHIP presenters against a vkrender receiver (Windows).

.DESCRIPTION
  M28: multi-presenter testing needs actual concurrent senders, and real phones
  don't scale to 12. This drives GStreamer's whipclientsink - the same WHIP
  protocol a browser, OBS or the Flutter sender speaks - so the receiver cannot
  tell these from real presenters.

  Each presenter gets a distinct videotestsrc pattern AND resolution, so the
  scene shows which is which and per-stream resolution negotiation is visible
  (every presenter should land at its own size, not a shared one).

.PARAMETER Count
  Number of concurrent presenters.

.PARAMETER Endpoint
  WHIP endpoint. Default matches the appliance's vkr_webrtc.

.PARAMETER Pin
  Sent as the Bearer token when the receiver has PIN auth enabled.

.PARAMETER Stop
  Kill every presenter this script started (matches on the pipeline tag).

.EXAMPLE
  scripts\whip_load_win.ps1 -Count 4
  scripts\whip_load_win.ps1 -Count 12 -Pin 1234
  scripts\whip_load_win.ps1 -Stop
#>
param(
    [int]$Count = 4,
    [string]$Endpoint = "http://127.0.0.1:8889/whip/endpoint",
    [string]$Pin = "",
    [switch]$Stop
)

$ErrorActionPreference = "Stop"

$gstBin = @(
    "C:\Program Files\gstreamer\1.0\msvc_x86_64\bin",
    "C:\gstreamer\1.0\msvc_x86_64\bin"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $gstBin) { throw "GStreamer MSVC runtime not found - install it from https://gstreamer.freedesktop.org/download/" }
$env:PATH = "$gstBin;$env:PATH"
$gstLaunch = Join-Path $gstBin "gst-launch-1.0.exe"

# Every presenter is named so -Stop can find exactly these and nothing else.
$tag = "vkrwhipload"

if ($Stop) {
    $killed = 0
    Get-CimInstance Win32_Process -Filter "Name = 'gst-launch-1.0.exe'" |
        Where-Object { $_.CommandLine -like "*$tag*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue; $killed++ }
    Write-Output "stopped $killed presenter(s)"
    return
}

# Distinct pattern + size per presenter. Sizes deliberately vary so the scene
# proves each stream negotiates its OWN resolution.
$profiles = @(
    @{ pattern = 0;  w = 1280; h = 720 },   # smpte
    @{ pattern = 18; w = 640;  h = 480 },   # ball (motion - shows dropped frames)
    @{ pattern = 1;  w = 1920; h = 1080 },  # snow (worst case for the encoder)
    @{ pattern = 19; w = 800;  h = 600 },   # smpte100
    @{ pattern = 4;  w = 1024; h = 768 },   # checkers-8
    @{ pattern = 22; w = 960;  h = 540 }    # colors
)

Write-Output "starting $Count presenter(s) -> $Endpoint"
for ($i = 0; $i -lt $Count; $i++) {
    $p = $profiles[$i % $profiles.Count]
    $signaller = "signaller::whip-endpoint=$Endpoint"
    if ($Pin) { $signaller += " signaller::auth-token=$Pin" }

    # is-live=true so the encoder is paced like a real capture rather than
    # racing ahead; zerolatency so behind-live numbers mean something.
    $pipeline = "videotestsrc name=$tag$i pattern=$($p.pattern) is-live=true " +
                "! video/x-raw,width=$($p.w),height=$($p.h),framerate=30/1 " +
                "! videoconvert ! whipclientsink $signaller"

    $log = Join-Path $env:TEMP "whip_presenter_$i.log"
    Start-Process -FilePath $gstLaunch -ArgumentList $pipeline `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" `
        -WindowStyle Hidden | Out-Null
    Write-Output ("  presenter {0}: pattern {1} at {2}x{3}" -f $i, $p.pattern, $p.w, $p.h)
    Start-Sleep -Milliseconds 400   # stagger: don't SDP-storm the receiver
}
Write-Output "logs: $env:TEMP\whip_presenter_*.log   (stop with -Stop)"
