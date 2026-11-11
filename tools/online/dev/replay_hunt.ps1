# The session that parted on 2026-11-09 (out\f3\desync_menu_20261109), played again: each player's buttons from its own
# recording, frame by frame (mk9_net_input_replay), so every run takes that session's path (the one-player menus).
# Runs it again and again until the two games part, keeping that run's checks and logs.
param([int]$Runs = 6, [int]$Seconds = 95, [string]$More = '', [string]$HostAffinity = '', [string]$GuestAffinity = '')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$src = Join-Path $root 'out\f3\desync_menu_20261109'
$f3 = Join-Path $root 'out\f3'
$logs = Join-Path $root 'out\pc\logs'
$extra = "--mk9_native_log_frames=3700-4430 $More".Trim()
for ($i = 1; $i -le $Runs; $i++) {
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\online\pair_test.ps1'),
              '-Seconds', "$Seconds", '-Checks', '-Extra', "`"$extra`"",
              '-HostExtra', "`"--mk9_net_input_replay=$src\session_20261109_182756_p1.mk9net --mk9_seed=1180407083`"",
              '-GuestExtra', "--mk9_net_input_replay=$src\session_20261109_182756_p2.mk9net")
    if ($HostAffinity) { $args += '-HostAffinity', $HostAffinity }
    if ($GuestAffinity) { $args += '-GuestAffinity', $GuestAffinity }
    & powershell @args | Out-Null
    $ds = (Select-String -Path (Join-Path $logs 'pair_host.log') -Pattern 'DESYNC|checks differ|games differ' -CaseSensitive -ErrorAction SilentlyContinue | Measure-Object).Count
    $modes = foreach ($role in 'host', 'guest') {
        ((Select-String -Path (Join-Path $f3 "$($role)_checks.txt") -Pattern '^(\d+) 9=\S+ 8=00000000' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ',')
    }
    $replay = (Select-String -Path (Join-Path $logs 'pair_host.log') -Pattern 'mk9_net_input_replay' | Select-Object -First 1).Line
    "run $i : modes host [$($modes[0])] guest [$($modes[1])]; desync lines $ds"
    if ($i -eq 1 -and $replay) { "  $($replay.Substring([Math]::Max(0, $replay.IndexOf('[net]'))))" }
    if ($ds -gt 0 -or $modes[0] -ne $modes[1]) {
        $keep = Join-Path $f3 ("replay_fail_{0:yyyyMMdd_HHmmss}" -f (Get-Date))
        New-Item -ItemType Directory -Force $keep | Out-Null
        foreach ($role in 'host', 'guest') {
            foreach ($kind in 'natives', 'draws', 'checks', 'scripts') {
                $file = Join-Path $f3 "$($role)_$kind.txt"
                if (Test-Path $file) { Copy-Item $file $keep }
            }
            Copy-Item (Join-Path $logs "pair_$role.log") $keep -ErrorAction SilentlyContinue
        }
        "the games parted in run ${i}: kept in $keep"
        exit 1
    }
}
"all $Runs runs stayed together"
