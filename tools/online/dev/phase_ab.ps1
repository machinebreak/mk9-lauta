# The phase alignment (PLAN-0.4 C1) on and off: the same fight session with each, and each side's rollbacks, frames run
# again and phase at the end, from the last [net] status line of each log (kept as out\f3\phase_<on|off>_<role>.log).
#
#   powershell -File tools\online\dev\phase_ab.ps1 [-Seconds 200] [-Lag 45] [-Jitter 10] [-Runs 1]
param([int]$Seconds = 200, [int]$Lag = 45, [int]$Jitter = 10, [int]$Runs = 1, [string]$More = "")
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$logs = Join-Path $root 'out\pc\logs'
$f3 = Join-Path $root 'out\f3'
for ($i = 1; $i -le $Runs; $i++) {
    foreach ($mode in 'off', 'on') {
        $align = if ($mode -eq 'on') { 'true' } else { 'false' }
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\online\pair_test.ps1') `
            -HostScript 'out\f3\host_long.txt' -GuestScript 'out\f3\guest_long.txt' -Seconds $Seconds `
            -Extra "--mk9_net_lag_ms=$Lag --mk9_net_jitter_ms=$Jitter --mk9_net_phase_align=$align $More" | Out-Null
        foreach ($role in 'host', 'guest') {
            $log = Join-Path $logs "pair_$role.log"
            Copy-Item $log (Join-Path $f3 "phase_$($mode)_$($role).log") -Force
            $last = Select-String -Path $log -Pattern 'epoch 1 frame .*rollback: ' | Select-Object -Last 1
            if (-not $last) { "run $i $mode ${role}: no fight status line"; continue }
            $line = $last.Line
            $rb = if ($line -match 'rollback: (\d+) rollbacks \((\d+) frames run again, deepest (\d+)\)') { "$($Matches[1]) rollbacks, $($Matches[2]) frames again, deepest $($Matches[3])" } else { '?' }
            $fr = if ($line -match 'epoch 1 frame (\d+)') { $Matches[1] } else { '?' }
            $rtt = if ($line -match 'rtt ([\d.]+) ms') { $Matches[1] } else { '?' }
            $ph = if ($line -match 'phase ([+-]?[\d.]+) ms ahead \(here ([+-]?[\d.]+), there ([+-]?[\d.]+)\), the grid moved back ([\d.]+) ms in (\d+) steps') { "phase $($Matches[1]) ms (here $($Matches[2]), there $($Matches[3])), moved $($Matches[4]) ms in $($Matches[5])" } else { '' }
            $dl = if ($line -match 'delay (\d+)') { $Matches[1] } else { '?' }
            # the fight's frame rate (the last minute's [fps] lines): under ~58 the measure says little
            $rates = Select-String -Path $log -Pattern '\[fps\] ([\d.]+) fps over' | Select-Object -Last 12 |
                ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
            $fps = if ($rates) { '{0:n1}' -f (($rates | Measure-Object -Average).Average) } else { '?' }
            "run $i $mode ${role}: fight frame $fr, rtt $rtt, delay ${dl}, $fps fps: $rb; $ph"
        }
    }
}
