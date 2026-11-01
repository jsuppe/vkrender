$ErrorActionPreference='Stop'
$d='C:/Users/jonsu/dev'
$b="$d/framelink-m5-final-validation/build-finite/Release"
$o="$d/m5-sustained-regression"
New-Item -ItemType Directory -Path $o -Force | Out-Null
$env:PATH="$b;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
& "$b/fl_av_offer_bounds.exe" *> "$o/bounds.log"
$bounds=$LASTEXITCODE
& python "$d/m5_finite_budget.py" --build $b --output "$o/finite" *> "$o/finite.log"
$finite=$LASTEXITCODE
@{bounds=$bounds;finite=$finite} | ConvertTo-Json | Set-Content "$o/exit-codes.json"
