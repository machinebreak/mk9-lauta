# The online's regression in one command (PLAN-0.4, A6): the rollback SyncTest, a hunt with a bad network, a spectator
# who joins late and catches up, and a whole room through the local lobby. It fails (exit 1) on a desync, a spectator
# that parts from the match, a crash or a hang. Before each release and after each task of the plan's phases A, B and
# C. Needs the local lobby (tools/online/dev/README.md: lobby_server.py 8791 --host 127.0.0.1) and nothing else running
# the build in out\pc.
#
#   powershell -File tools\online\regress.ps1 [-Quick]   (-Quick: one hunt run instead of four, a shorter SyncTest)
param([switch]$Quick)
$ErrorActionPreference = 'Continue'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$logs = Join-Path $root 'out\pc\logs'
$started = Get-Date
$fails = New-Object System.Collections.Generic.List[string]
function Step([string]$name) { "== $(Get-Date -Format HH:mm:ss) $name" }
function Since([string]$path, [string]$pattern) {
    # lines matching pattern written since the regression started (logs that append keep older runs)
    if (-not (Test-Path $path)) { return @() }
    return @(Select-String -Path $path -Pattern $pattern | Where-Object {
        $_.Line -match '^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)' -and [datetime]$Matches[1] -ge $started.AddSeconds(-1)
    })
}

# 1. The SyncTest: a fight against the AI whose frames run again every 12 frames, 6 back (the clock made of frames on)
Step 'SyncTest'
$synctest = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'synctest_runs.ps1') -Runs 1 `
    -Seconds $(if ($Quick) { 125 } else { 180 })
$synctest
if (-not ($synctest -match '\[synctest\] \d+ rollbacks') -or ($synctest -match 'DIFFERS|access violation|FATAL')) {
    $fails.Add("SyncTest: $synctest")
}

# 2. The hunt: two games over local UDP with 60 +- 40 ms and 2 % loss, again and again
Step 'hunt'
$hunt = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hunt.ps1') `
    -Runs $(if ($Quick) { 1 } else { 4 }) -Seconds 150 -NativeFrames '3700-4100' `
    -Extra '--mk9_net_lag_ms=60 --mk9_net_jitter_ms=40 --mk9_net_loss_pct=2'
$hunt
if ($LASTEXITCODE -ne 0 -or -not ($hunt -match 'stayed together')) { $fails.Add("hunt: $($hunt | Select-Object -Last 2)") }

# 3. A spectator 95 s late: it catches up in the menus and in the fight and must stay with the match
Step 'spectator'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'dev\fight_watch.ps1') -Seconds 260 `
    -WatchDelay 95 | Out-Null
$watch = Join-Path $logs 'watch.log'
$parted = Since $watch 'differs from the match|went another way'
$caught = Since $watch '\[watch\] epoch 1 at'
"spectator: $($parted.Count) lines parted from the match; reached the fight: $($caught.Count -gt 0)"
if ($parted.Count -gt 0) { $fails.Add("spectator: $($parted[0].Line)") }
if ($caught.Count -eq 0) { $fails.Add('spectator: never reached the fight') }
foreach ($role in 'host', 'guest') {
    $d = Since (Join-Path $logs "pair_$role.log") 'DESYNC'
    if ($d.Count -gt 0) { $fails.Add("spectator's players ($role): $($d[0].Line)") }
}

# 4. A whole room through the local lobby: meet, ready, start again joined, fight over ICE
Step 'room'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'room_test.ps1') | Out-Null
foreach ($role in 'host', 'guest') {
    $log = Join-Path $logs "room_$role.log"
    $d = Since $log 'DESYNC'
    $fight = Since $log "barrier 'fight start'"
    "room ${role}: fight started $($fight.Count -gt 0), desync lines $($d.Count)"
    if ($d.Count -gt 0) { $fails.Add("room ($role): $($d[0].Line)") }
    if ($fight.Count -eq 0) { $fails.Add("room ($role): the fight never started") }
}

# Crashes and hangs anywhere in the run
$dumps = @(Get-ChildItem (Join-Path $logs 'crash_*.dmp') -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -ge $started })
if ($dumps.Count -gt 0) { $fails.Add("crash dumps: $($dumps.Name -join ', ')") }
$hangs = @(Get-ChildItem (Join-Path $logs '*.log') | Where-Object { $_.LastWriteTime -ge $started } | ForEach-Object { Since $_.FullName '\[hang\]' })
if ($hangs.Count -gt 0) { $fails.Add("hangs: $($hangs[0].Line)") }

Step 'done'
if ($fails.Count -gt 0) {
    "REGRESSION FAILED ($($fails.Count)):"
    $fails | ForEach-Object { "  - $_" }
    exit 1
}
"regression clean ($([int]((Get-Date) - $started).TotalMinutes) min)"
