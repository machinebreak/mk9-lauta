# The online's warnings (PLAN-0.4 C4/C5) on screen: two games with a long round trip (the menus' "slow connection"),
# then the guest killed: the host's "no word from" with its countdown, and the session's end. In a room of the local
# lobby (the badge is a room session's). The host's captures
# (out\pc\captures\snap_<s>s.png): -MenuSnap in the menus, -KillAt + 12 during the countdown.
#
#   powershell -File tools\online\dev\silent_peer.ps1 [-Lag 150] [-MenuSnap 40] [-KillAt 95] [-Seconds 130]
param([int]$Lag = 150, [int]$MenuSnap = 40, [int]$KillAt = 95, [int]$Seconds = 130)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$exe = Join-Path $root 'out\pc\mk9.exe'
$snaps = "$MenuSnap,$($KillAt + 12)"
$pair = Start-Process -FilePath powershell.exe -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\online\pair_test.ps1'),
    '-Seconds', "$Seconds", '-HostScript', 'out\f3\host_fight.txt', '-GuestScript', 'out\f3\guest_fight.txt',
    '-Snapshots', $snaps, '-Room', 'SILENT1', '-Extra', "`"--mk9_net_lag_ms=$Lag --renut_capture_overlay=true`"")
Start-Sleep -Seconds $KillAt
$guest = Get-CimInstance Win32_Process -Filter "Name='mk9.exe'" |
    Where-Object { $_.ExecutablePath -eq $exe -and $_.CommandLine -match 'pair_guest' } | Select-Object -First 1
if ($guest) {
    Stop-Process -Id $guest.ProcessId -Force
    "guest killed at $KillAt s"
} else {
    'no guest to kill'
}
$pair.WaitForExit(($Seconds + 60) * 1000) | Out-Null
"done"
