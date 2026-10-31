$ErrorActionPreference='Stop'
$d='C:/Users/jonsu/dev'
xperf -on PROC_THREAD+LOADER+CSWITCH+DISPATCHER -stackwalk CSwitch+ReadyThread -buffersize 1024 -minbuffers 64 -maxbuffers 128 -f "$d/m5-present-etw-baseline.etl"
if ($LASTEXITCODE -ne 0) { throw "ETW start failed" }
$a=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $d/m5-present-causal-capture.ps1"
$p=New-ScheduledTaskPrincipal -UserId 'jonsu' -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName 'FrameLink-M5-Present-Causal' -Action $a -Principal $p -Force | Out-Null
Start-ScheduledTask -TaskName 'FrameLink-M5-Present-Causal'
