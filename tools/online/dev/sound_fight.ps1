# Two players fight (Versus, real inputs: out\f3\host_long.txt / guest_long.txt) over direct UDP with emulated latency,
# each writing its sounds' callbacks (mk9_sound_log) and checks: what the fight's sound does on each machine.
param([int]$Seconds = 200, [string]$Lag = '40', [string]$Jitter = '10', [string]$More = '')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$f3 = Join-Path $root 'out\f3'
$extra = "--mk9_net_lag_ms=$Lag --mk9_net_jitter_ms=$Jitter $More".Trim()
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\online\pair_test.ps1') -Seconds $Seconds `
    -HostScript 'out\f3\host_long.txt' -GuestScript 'out\f3\guest_long.txt' -Checks -Extra $extra `
    -HostExtra "--mk9_sound_log=$f3\host_sound.txt" -GuestExtra "--mk9_sound_log=$f3\guest_sound.txt" | Out-Null
foreach ($role in 'host', 'guest') {
    $log = Join-Path $root "out\pc\logs\pair_$role.log"
    "== $role"
    Select-String -Path $log -Pattern 'checks differ|DESYNC|mode ended|barrier .fight|no new FMOD|sound sync|\[sound\]' |
        Select-Object -First 30 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(210, $_.Line.Length)) }
}
