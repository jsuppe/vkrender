$d='C:/Users/jonsu/dev';$r="$d/vkrender-m5-final-validation/build-final"
$env:PATH="$r/Release;C:/Program Files/gstreamer/1.0/msvc_x86_64/bin;"+$env:PATH
$n='m5-present-power-default';$state="$d/$n-power.jsonl";$stop="$d/$n-observer-stop"
$observer=Start-Process python -ArgumentList @("$d/m5-power-observer.py",$state,'480',$stop) -PassThru -RedirectStandardError "$d/$n-observer.err"
try {
 $deadline=[DateTime]::UtcNow.AddSeconds(400)
 $off=$false
 while ([DateTime]::UtcNow -lt $deadline -and -not $observer.HasExited) {
  if(Test-Path $state){$lines=Get-Content $state; $latest=$lines | ForEach-Object {$_ | ConvertFrom-Json} | Where-Object event -eq display_state | Select-Object -Last 1
   if($latest -and $latest.value -eq 0){$off=$true;break}}
  Start-Sleep -Milliseconds 250
 }
 if(-not $off){throw 'No naturally-off display state observed; no media launched'}
 python "$d/native_av_preparation.py" --build "$r/framelink/Release" --renderer "$r/Release/vkrenderer.exe" --output "$d/$n" --case prepared *> "$d/$n.log"
 @{exit=$LASTEXITCODE;finished=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content "$d/$n.done"
} catch {$_ | Out-File "$d/$n.error"} finally {'stop'|Set-Content $stop; $observer.WaitForExit()}
