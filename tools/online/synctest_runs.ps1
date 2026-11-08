# Runs the rollback SyncTest (mk9_synctest, app/src/online/snapshot.cpp) on a fight against the AI several times and
# prints each run's last [synctest] line, or how it ended (a crash, a hang): rollback's stability.
#
#   powershell -File tools\online\synctest_runs.ps1 [-Runs 4] [-Seconds 125] [-Spec "4700,12,6"] [-Set "name=value;..."]
param(
    [int]$Runs = 4,
    [int]$Seconds = 125,
    [string]$Spec = '4700,12,6',
    [string]$Set = 'mk9_timeline_foreign=false'
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$logs = Join-Path $root 'out\pc\logs'
for ($i = 1; $i -le $Runs; $i++) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'run_game.ps1') -Seconds $Seconds `
        -Script '32 START;38 A;44 A;52 A;58 A;64 A;70 A;76 A' -Set "mk9_synctest=$Spec;$Set" -Tail 1 | Out-Null
    $log = (Get-ChildItem (Join-Path $logs 'mk9_*.log') | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
    $last = (Select-String -Path $log -Pattern '\[synctest\] \d+ rollbacks' | Select-Object -Last 1)
    $bad = (Select-String -Path $log -Pattern 'access violation|FATAL|DIFFERS' | Select-Object -First 1)
    $fps = (Select-String -Path $log -Pattern '\[fps\]' | Select-Object -Last 1)
    $text = if ($last) { $last.Line.Substring($last.Line.IndexOf('[synctest]')) } else { 'no synctest line' }
    if ($bad) { $text += ' | ' + $bad.Line.Substring([Math]::Min(60, $bad.Line.Length)) }
    if ($fps) { $text += ' | last ' + $fps.Line.Substring($fps.Line.IndexOf('[fps]')) }
    "run ${i}: $text"
}
