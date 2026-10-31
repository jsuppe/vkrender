$ErrorActionPreference='Stop'
$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final";$o="$d/m5-sustained-preparation"
New-Item -ItemType Directory -Path $o -Force | Out-Null
$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$env:VK_LAYER_PATH="$d/vulkan-validation-local/Bin";$env:VKR_VALIDATION='1'
$results=@()
foreach($case in @('prepared','failure','timeout')) {
 & python "$d/native_av_preparation.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output "$o/$case" --case $case --media-run-ms 70000 --require-early-power *> "$o/$case.log"
 $code=$LASTEXITCODE;$results+=@{case=$case;code=$code};$results | ConvertTo-Json | Set-Content "$o/exit-codes.json"
 if($code -ne 0){break}
}
