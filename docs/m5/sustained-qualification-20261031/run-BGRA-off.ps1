$ErrorActionPreference='Stop'
$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final"
$o="$d/m5-sustained-BGRA-off"
New-Item -ItemType Directory -Path $o -Force | Out-Null
$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
& python "$d/m5_renderer_rate_probe.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output $o --native --seconds 70 --clock-policy producer --trace off --case 1920x1080-60-BGRA-av *> "$o/harness.log"
@{harness=$LASTEXITCODE} | ConvertTo-Json | Set-Content "$o/exit-codes.json"
