# Short sessions (through the first seconds of the first fight, where what the fight streamed in ends loading) with the
# fighters dumped every frame (both moments: the boundary's start and the sync hash's) and the game's checks on.
# Without -KeepGoing it stops at the first run whose fighter hash differs ("the fighters' state differs") and keeps that run's files
# as out\f3\intro_<role>_{checks,draws}.txt and out\pc\logs\intro_pair_<role>.log; with it, every run's dumps stay as
# out\f3\intro_<n>_<role>_dump.bin for tools/online/dev/fighter_diff.py.
#
#   powershell -File tools\online\dev\intro_hunt.ps1 [-Runs 8] [-Seconds 90] [-Extra "--mk9_net_lag_ms=40 ..."] [-KeepGoing]
param([int]$Runs = 8, [int]$Seconds = 90, [string]$Extra = '--mk9_net_lag_ms=40 --mk9_net_jitter_ms=10',
      [switch]$KeepGoing)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$f3 = Join-Path $root 'out\f3'
$logs = Join-Path $root 'out\pc\logs'
$parted = 0
for ($i = 1; $i -le $Runs; $i++) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\online\pair_test.ps1') `
        -HostScript 'out\f3\host_fight.txt' -GuestScript 'out\f3\guest_fight.txt' -Seconds $Seconds -Checks `
        -Extra "$Extra --mk9_fighter_dump_every=1" `
        -HostExtra "--mk9_fighter_dump=$f3\intro_host_dump.bin" -GuestExtra "--mk9_fighter_dump=$f3\intro_guest_dump.bin" | Out-Null
    $d = (Select-String -Path (Join-Path $logs 'pair_host.log') -Pattern "fighters' state differs|DESYNC in" -CaseSensitive -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($KeepGoing) {
        foreach ($role in 'host', 'guest') {
            Move-Item (Join-Path $f3 "intro_$($role)_dump.bin") (Join-Path $f3 "intro_$($i)_$($role)_dump.bin") -Force
        }
    }
    if ($d) {
        $parted++
        "run ${i}: $($d.Line.Substring($d.Line.IndexOf('[net]')))"
        if (-not $KeepGoing) {
            foreach ($role in 'host', 'guest') {
                foreach ($kind in 'checks', 'draws') {
                    Copy-Item (Join-Path $f3 "$($role)_$kind.txt") (Join-Path $f3 "intro_$($role)_$kind.txt") -Force
                }
                Copy-Item (Join-Path $logs "pair_$role.log") (Join-Path $logs "intro_pair_$role.log") -Force
            }
            exit 1
        }
    } else {
        "run ${i}: the fighter hash stayed the same"
    }
}
if ($parted) { exit 1 }
exit 0
