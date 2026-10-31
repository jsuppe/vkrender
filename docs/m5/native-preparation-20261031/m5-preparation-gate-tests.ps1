$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final";$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
python "$d/native_av_preparation.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output "$d/m5-native-preparation-headless" --headless *> "$d/m5-native-preparation-headless.log"
@{headless=$LASTEXITCODE}|ConvertTo-Json|Set-Content "$d/m5-native-preparation-headless-result.json"
foreach($case in @('failure','timeout')){
 python "$d/native_av_preparation.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output "$d/m5-native-preparation-coherent" --case $case *> "$d/m5-native-preparation-coherent-$case.log"
 @{case=$case;exit=$LASTEXITCODE}|ConvertTo-Json -Compress|Add-Content "$d/m5-native-preparation-coherent-results.jsonl"
}
