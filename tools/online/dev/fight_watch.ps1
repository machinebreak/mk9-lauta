# The players fight with real inputs (out\f3\host_long.txt / guest_long.txt: a Versus, fight after fight) over room
# WATCH1 with emulated latency (rollbacks), and a spectator watches from the start (watcher.ps1).
param([int]$Seconds = 260, [int]$WatchDelay = 3, [string]$Lag = '40', [string]$Jitter = '10', [string]$More = '')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$scratch = $PSScriptRoot
$pair = Start-Process -FilePath powershell.exe -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\online\pair_test.ps1'),
    '-Room', 'WATCH1', '-Checks', '-Seconds', "$Seconds", '-HostScript', 'out\f3\host_long.txt',
    '-GuestScript', 'out\f3\guest_long.txt', '-Extra', "`"--mk9_net_lag_ms=$Lag --mk9_net_jitter_ms=$Jitter $More`"")
& (Join-Path $scratch 'watcher.ps1') -Delay $WatchDelay -Seconds ($Seconds - $WatchDelay) -Extra $More
$pair.WaitForExit(($Seconds + 60) * 1000) | Out-Null
"pair done: $($pair.HasExited)"
