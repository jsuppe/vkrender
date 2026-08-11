<#
.SYNOPSIS
  Build the appliance web payload: the browser sender + desktop downloads.

.DESCRIPTION
  M41: stages installer/payload/webroot from fresh Flutter builds -
    webroot/               the sender's web build (served at / by vkr_web)
    webroot/downloads/     vkrender-sender-windows.zip + README

  The payload is NOT committed (60 MB of build products); run this before
  compiling the installer. The macOS zip is added by hand when a macOS build
  host produces one - the README in downloads/ explains its absence honestly.
#>
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sender = Join-Path $repo "sender"
$payload = Join-Path $repo "installer\payload\webroot"

Push-Location $sender
try {
    flutter build web --release
    flutter build windows --release
} finally { Pop-Location }

if (Test-Path $payload) { Remove-Item -Recurse -Force $payload }
New-Item -ItemType Directory -Force (Join-Path $payload "downloads") | Out-Null
Copy-Item -Recurse (Join-Path $sender "build\web\*") $payload
Compress-Archive -Path (Join-Path $sender "build\windows\x64\runner\Release\*") `
    -DestinationPath (Join-Path $payload "downloads\vkrender-sender-windows.zip") -Force

@"
vkrender sender - desktop applications
======================================

vkrender-sender-windows.zip   Windows 10/11 x64. Unzip anywhere and run
                              vkrender_sender.exe. It discovers receivers on
                              the network over mDNS, or takes an address.

vkrender-sender-macos.zip     Not built yet - requires a macOS build host.
                              Mac users can cast today without any download:
                              this appliance appears in Control Center >
                              Screen Mirroring (AirPlay), and the browser
                              sender at this server's https address works in
                              Safari and Chrome.

The browser sender (this server's front page, over https) casts from any
machine with Chrome, Edge or Safari - no install at all.
"@ | Out-File -Encoding utf8 (Join-Path $payload "downloads\README.txt")

Write-Output "payload staged at $payload"
