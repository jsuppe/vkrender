$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final"
$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$env:VK_LAYER_PATH="$d/vulkan-validation-local/Bin";$env:VKR_VALIDATION='1'
python "$d/native_av_preparation.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output "$d/m5-power-fixed-headless" --case prepared --require-early-power --headless *> "$d/m5-power-fixed-headless.log"
@{exit=$LASTEXITCODE}|ConvertTo-Json|Set-Content "$d/m5-power-fixed-headless.done"
