$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final";$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
ctest --test-dir "$d/framelink-m5-capacity-tests" -C Release -R '^expanded_pool$' --output-on-failure *> "$d/m5-capacity-expanded-pool.log"
$code=$LASTEXITCODE
@{expanded_pool=$code}|ConvertTo-Json|Set-Content "$d/m5-capacity-gate.json"
if($code -eq 0) { & "$d/m5-capacity-matrix.ps1" }
