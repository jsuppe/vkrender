# Renderer rate cell: producer -> vkrenderer --serve -> scene outputs -> checker,
# at the FrameLink target sizes and rates (1920x1080@60, 3840x2160@30).
#
# Gates, matching FrameLink's primary cells: zero output arrivals over the frame
# budget (17 ms at 60 fps, 34 ms at 30 fps) and zero output sequence gaps over
# the measured window; every steady-state 5 s renderer window adopts every input
# frame and its producer-to-adoption hop stays inside the budget; sampled output
# pixels match within the BT.709 limit; and no process grows private memory by
# more than 64 KB/min. Qualification uses -Seconds 1800; shorter runs are smoke
# checks only.
param(
 [Parameter(Mandatory=$true)][string]$RendererBuild,
 [ValidateSet('1080p60','4k30')][string]$Cell='1080p60',
 [ValidateSet('BGRA','NV12')][string]$Format='BGRA',
 [ValidateSet('cpu','gpu')][string]$OutputMode='gpu',
 [int]$Seconds=1800,
 [int]$Warmup=10,
 [int]$VerifyEvery=0,
 [string]$Out
)
$ErrorActionPreference='Stop'
$r=(Resolve-Path $RendererBuild).Path; $bin="$r\Release"
$w,$h,$fps,$budget = if($Cell -eq '1080p60'){1920,1080,60,17}else{3840,2160,30,34}
if(!$VerifyEvery){$VerifyEvery=$fps} # about one verified frame per second
$name="rate-$Cell-$($Format.ToLower())-$OutputMode-$PID"
if(!$Out){$Out=Join-Path $r $name}
New-Item -ItemType Directory -Force $Out|Out-Null; $Out=(Resolve-Path $Out).Path
$env:PATH="$bin;"+$env:PATH
$env:VKR_INPUT_FORMATS=$Format.ToLower()
$env:VKR_OUTPUT_MODE=if($OutputMode -eq 'gpu'){'gpu'}else{''}
$env:VKR_TRACE='';$env:FRAMELINK_TRACE=''
$env:VKR_CHECK_WIDTH="$w";$env:VKR_CHECK_HEIGHT="$h"
$env:VKR_CHECK_SECONDS="$Seconds";$env:VKR_CHECK_WARMUP="$Warmup";$env:VKR_CHECK_BUDGET_MS="$budget"
$env:VKR_CHECK_VERIFY_EVERY="$VerifyEvery";$env:VKR_CHECK_FRAMES=[string][math]::Max(1,[math]::Floor($Seconds/2))
$env:VKR_CHECK_TIMEOUT=[string]($Seconds+$Warmup+120)
$frames=($Seconds+$Warmup+60)*$fps
$cfg=[ordered]@{cell=$Cell;width=$w;height=$h;fps=$fps;budget_ms=$budget;input_format=$Format;output_mode=$OutputMode;
 seconds=$Seconds;warmup=$Warmup;verify_every=$VerifyEvery;producer_frames=$frames}
$procs=[ordered]@{};$memory=[Collections.Generic.List[object]]::new()
function Start-Logged($key,$exe,$argList){
 $p=Start-Process $exe -ArgumentList $argList -WorkingDirectory $Out -PassThru -NoNewWindow `
   -RedirectStandardOutput "$Out\$key.out" -RedirectStandardError "$Out\$key.err"
 $null=$p.Handle; $procs[$key]=$p; return $p
}
$result=[ordered]@{config=$cfg}
try {
 $check=Start-Logged 'check' "$bin\vkr_portable_output_check.exe" $name
 Start-Sleep -Milliseconds 300
 $render=Start-Logged 'render' "$bin\vkrenderer.exe" "--serve $name --scene-output $name.rgba --scene-output $name.nv12 --headless -1 --window ${w}x${h}+0+0 --control-port 0"
 Start-Sleep -Milliseconds 800
 $producer=Start-Logged 'producer' "$bin\framelink_producer.exe" "--channel $name.in.0 --format $($Format.ToLower()) --size ${w}x${h} --fps $fps --frames $frames --deadline-ms 20000"
 $t0=Get-Date; $limit=$Seconds+$Warmup+150
 while(!$check.HasExited){
  if(((Get-Date)-$t0).TotalSeconds -gt $limit){throw 'checker timeout'}
  $row=[ordered]@{t=[math]::Round(((Get-Date)-$t0).TotalSeconds,1)}
  foreach($k in 'render','producer','check'){
   $p=$procs[$k]; if($p.HasExited){continue}; $p.Refresh(); $row["${k}_private"]=$p.PrivateMemorySize64; $row["${k}_ws"]=$p.WorkingSet64
  }
  $memory.Add([pscustomobject]$row)
  $check.WaitForExit(10000)|Out-Null
 }
 $result.check_exit=$check.ExitCode
} catch { $result.error="$_" }
finally {
 foreach($k in 'producer','render','check'){$p=$procs[$k];if($p -and !$p.HasExited){Stop-Process -Id $p.Id -Force}}
}
$memory|Export-Csv -NoTypeInformation "$Out\memory.csv"
$checkOut=Get-Content "$Out\check.out" -EA 0
$result.check=$checkOut
# Memory slope (KB/min) per process over the measured window, least squares.
$slopes=[ordered]@{}
$rows=@($memory|Where-Object{$_.t -ge $Warmup+30})
foreach($k in 'render','producer','check'){
 $pts=@($rows|Where-Object{$_."${k}_private"}|ForEach-Object{,@([double]$_.t,[double]$_."${k}_private")})
 if($pts.Count -lt 3){$slopes[$k]=$null;continue}
 $n=$pts.Count;$mx=($pts|%{$_[0]}|Measure-Object -Average).Average;$my=($pts|%{$_[1]}|Measure-Object -Average).Average
 $num=0.0;$den=0.0;foreach($q in $pts){$num+=($q[0]-$mx)*($q[1]-$my);$den+=($q[0]-$mx)*($q[0]-$mx)}
 $slopes[$k]=[math]::Round(($num/$den)*60/1024,2)
}
$result.memory_kb_per_min=$slopes
# Renderer input side: steady-state 5 s windows (skip the first two and the last).
$win=@(Select-String -Path "$Out\render.err" -Pattern 'service: (\d+) adoptions in 5s \(pool[^)]*\)(?: hop avg ([\d.]+) ms max ([\d.]+) ms)?' -EA 0|ForEach-Object{
 [pscustomobject]@{adoptions=[int]$_.Matches[0].Groups[1].Value;hopMax=if($_.Matches[0].Groups[3].Success){[double]$_.Matches[0].Groups[3].Value}else{$null}}})
$steady=if($win.Count -gt 3){$win[2..($win.Count-2)]}else{@()}
$minAdopt=if($steady){($steady|Measure-Object adoptions -Minimum).Minimum}else{0}
$hopMax=if($steady|Where-Object hopMax){($steady|Where-Object hopMax|Measure-Object hopMax -Maximum).Maximum}else{$null}
$result.input=[ordered]@{windows=$steady.Count;min_adoptions_per_5s=$minAdopt;expected_per_5s=$fps*5;hop_max_ms=$hopMax}
$result.producer=(Get-Content "$Out\producer.err" -EA 0|Select-String 'producer (sent|FAIL)'|Select-Object -Last 1|%{$_.Line})
$fail=[Collections.Generic.List[string]]::new()
if($result.error){$fail.Add($result.error)}
if($result.check_exit -ne 0){$fail.Add("checker exit $($result.check_exit)")}
if(!$steady.Count){$fail.Add('no steady-state renderer windows')}
elseif($minAdopt -lt $fps*5-1){$fail.Add("input drops: min $minAdopt adoptions per 5 s, expected $($fps*5)")}
if($null -ne $hopMax -and $hopMax -gt $budget){$fail.Add("input hop max $hopMax ms over $budget ms")}
foreach($k in $slopes.Keys){if($null -ne $slopes[$k] -and $slopes[$k] -gt 64){$fail.Add("$k memory +$($slopes[$k]) KB/min")}}
$result.pass=($fail.Count -eq 0);$result.failures=$fail
$result|ConvertTo-Json -Depth 5|Set-Content "$Out\summary.json"
$checkOut|ForEach-Object{Write-Output "  $_"}
Write-Output ("  input: min {0}/{1} adoptions per 5 s over {2} windows, hop max {3} ms; memory KB/min: {4}" -f $minAdopt,($fps*5),$steady.Count,$hopMax,(($slopes.GetEnumerator()|%{"$($_.Key)=$($_.Value)"}) -join ' '))
if($fail.Count){throw ("FAIL ${Cell} $Format ${OutputMode}: "+($fail -join '; '))}
Write-Output "PASS: $Cell $Format $OutputMode, $Seconds s, summary $Out\summary.json"
