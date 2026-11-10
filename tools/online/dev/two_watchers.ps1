# The players fight (Versus, real inputs) over room WATCH1, one spectator watches live from the start ('live') and
# another joins late and catches up ('late'): their draws, checks and natives compared frame by frame show what catching
# up changes (neither has frames run again).
param([int]$Seconds = 230, [int]$LateDelay = 95, [string]$More = '')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$scratch = $PSScriptRoot
$pair = Start-Process -FilePath powershell.exe -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\online\pair_test.ps1'),
    '-Room', 'WATCH1', '-Seconds', "$Seconds", '-HostScript', 'out\f3\host_long.txt',
    '-GuestScript', 'out\f3\guest_long.txt', '-Extra', "`"--mk9_net_lag_ms=40 --mk9_net_jitter_ms=10 $More`"")
$live = Start-Process -FilePath powershell.exe -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $scratch 'watcher.ps1'), '-Delay', '3',
    '-Seconds', "$($Seconds - 3)", '-Name', 'live', '-Extra', "`"$More`"")
& (Join-Path $scratch 'watcher.ps1') -Delay $LateDelay -Seconds ($Seconds - $LateDelay) -Name 'late' -Extra $More | Out-Null
$live.WaitForExit(($Seconds + 60) * 1000) | Out-Null
$pair.WaitForExit(($Seconds + 60) * 1000) | Out-Null
"done"
