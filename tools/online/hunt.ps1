# Runs tools\online\pair_test.ps1 again and again until the two machines part: the fight starts at different frames
# (the session's barrier), a mode starts at different frames (the game's check tag 8 back to 0) or a frame's sync
# hashes differ (DESYNC). The failing run's logs are kept as out\f3\fail_<role>_{natives,draws,checks,scripts}.txt
# and out\pc\logs\fail_pair_<role>.log, to compare (tools\online\rand_report.py, the natives of the window).
#
#   powershell -File tools\online\hunt.ps1 [-Runs 6] [-Seconds 112] [-HostScript out\f3\host_fight.txt]
#       [-GuestScript out\f3\guest_fight.txt] [-NativeFrames "3700-4430"] [-Extra "--mk9_net_lag_ms=30 ..."]
param(
    [int]$Runs = 6,
    [int]$Seconds = 112,
    [string]$HostScript = 'out\f3\host_fight.txt',
    [string]$GuestScript = 'out\f3\guest_fight.txt',
    [string]$NativeFrames = '3700-4430',
    [string]$Extra = ''
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$f3 = Join-Path $root 'out\f3'
$logs = Join-Path $root 'out\pc\logs'

function First-Match([string]$path, [string]$pattern) {
    if (-not (Test-Path $path)) { return '' }
    $m = Select-String -Path $path -Pattern $pattern | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value } else { return '' }
}

function Mode-Starts([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    return ((Select-String -Path $path -Pattern '^(\d+) 9=\S+ 8=00000000' | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ',')
}

for ($i = 1; $i -le $Runs; $i++) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'pair_test.ps1') -HostScript $HostScript `
        -GuestScript $GuestScript -Seconds $Seconds -Checks -Extra "--mk9_native_log_frames=$NativeFrames $Extra".Trim() | Out-Null
    $hb = First-Match (Join-Path $logs 'pair_host.log') "barrier 'fight start' at frame (\d+)"
    $gb = First-Match (Join-Path $logs 'pair_guest.log') "barrier 'fight start' at frame (\d+)"
    $hm = Mode-Starts (Join-Path $f3 'host_checks.txt')
    $gm = Mode-Starts (Join-Path $f3 'guest_checks.txt')
    $ds = (Select-String -Path (Join-Path $logs 'pair_host.log') -Pattern 'DESYNC' -ErrorAction SilentlyContinue | Measure-Object).Count
    $ep = First-Match (Join-Path $logs 'pair_host.log') 'frame (\d+) \(game \d+\), rtt'
    "run $i : fight start host $hb guest $gb; mode starts host [$hm] guest [$gm]; desync lines $ds"
    if ($hb -ne $gb -or $hm -ne $gm -or $ds -gt 0) {
        foreach ($role in 'host', 'guest') {
            foreach ($kind in 'natives', 'draws', 'checks', 'scripts') {
                $src = Join-Path $f3 "$($role)_$kind.txt"
                if (Test-Path $src) { Copy-Item $src (Join-Path $f3 "fail_$($role)_$kind.txt") -Force }
            }
            $log = Join-Path $logs "pair_$role.log"
            if (Test-Path $log) { Copy-Item $log (Join-Path $logs "fail_pair_$role.log") -Force }
        }
        "the machines parted in run ${i}; logs kept as out\f3\fail_*"
        exit 1
    }
}
"all $Runs runs stayed together"
