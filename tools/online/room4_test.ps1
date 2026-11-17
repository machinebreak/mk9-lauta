# mk9-lauta (2026-11-17): a room of four end to end on this machine: player 1 opens a public room of four
# (mk9_online_test=host4), the others join it from the list (=quick4), everyone says ready, the host starts once all of
# them are in (mk9_online_test_start), and every game starts again already joined (session.h: a session of that many
# machines, a path between each two, ice_link.h) to play the scripts. A local lobby is expected (ps2-web-recomp's
# scripts/lobby_server.py 8791 --host 127.0.0.1): never the real one. Logs: out\pc\logs\room4_<n>.log (each game's;
# the one started again appends to it).
#
#   powershell -File tools\online\room4_test.ps1 [-Players 4] [-Seconds 260] [-Mode four|koth]
#       [-Scripts "out\f3\mesh4_p1.txt,...,out\f3\mesh4_p4.txt"] [-Extra "--name=value ..."]
#
# -Mode koth (2026-11-17): a room of King of the Hill instead (mk9_online_test=hostk, the others =joink); its scripts:
# tools\online\gen_koth_scripts.py's out\f3\koth_p1.txt ...
# -Logs (2026-11-17): each player's game writes the game's checks, the script functions it starts and (in the game
# frames of -NativeFrames, "a-b") the script natives to out\f3\room_{checks,scripts,natives}_<n>.txt: through the
# environment (REX_MK9_*), which the game started again for the session keeps (its command line drops test outputs).
# tools\online\dev\cmp_checks.py compares two players' checks.
param(
    [ValidateRange(2, 4)][int]$Players = 4,
    [ValidateSet('four', 'koth')][string]$Mode = 'four',
    [int]$Seconds = 260,
    [string]$Lobby = 'ws://127.0.0.1:8791/lobby?game=mk9pc',
    [string]$Scripts = 'out\f3\mesh4_p1.txt,out\f3\mesh4_p2.txt,out\f3\mesh4_p3.txt,out\f3\mesh4_p4.txt',
    [string]$Extra = '',
    [switch]$Logs,
    [string]$NativeFrames = ''
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dir = Join-Path $root 'out\pc'
$exe = Join-Path $dir 'mk9.exe'
$scriptList = $Scripts -split ','
function Stop-Games {
    Get-CimInstance Win32_Process -Filter "Name='mk9.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -eq $exe } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
Stop-Games
Remove-Item (Join-Path $dir 'logs\room4_*.log') -ErrorAction SilentlyContinue
function Args-For([int]$n) {
    $a = @('--audio_mute=true', '--window_start_minimized=true', '--sa2_renderer=renut', '--renut_gpu_backend=d3d12',
           "--log_file=logs/room4_$n.log", "--mk9_lobby_url=$Lobby", '--mk9_discord_presence=false',
           "--mk9_online_test=$(if ($Mode -eq 'koth') { if ($n -eq 0) { 'hostk' } else { 'joink' } } elseif ($n -eq 0) { 'host4' } else { 'quick4' })",
           "--mk9_online_test_start=$Players",
           "--mk9_online_name=Prueba$($n + 1)", '--mk9_net_skip_logos=false', '--mk9_input_script_frames=true')
    if ($n -lt $scriptList.Count -and $scriptList[$n]) { $a += "--mk9_input_script_file=$(Join-Path $root $scriptList[$n])" }
    if ($Extra) { $a += ($Extra -split ' ') }
    return ($a -join ' ')
}
for ($n = 0; $n -lt $Players; ++$n) {
    if ($Logs) {
        $env:REX_MK9_CHECK_LOG = Join-Path $root "out\f3\room_checks_$n.txt"
        $env:REX_MK9_SCRIPT_LOG = Join-Path $root "out\f3\room_scripts_$n.txt"
        if ($NativeFrames) {
            $env:REX_MK9_NATIVE_LOG = Join-Path $root "out\f3\room_natives_$n.txt"
            $env:REX_MK9_NATIVE_LOG_FRAMES = $NativeFrames
        }
    }
    Start-Process -FilePath $exe -WorkingDirectory $dir -WindowStyle Minimized -ArgumentList (Args-For $n) | Out-Null
    # the host's room first in the list; then the others, a moment apart
    Start-Sleep -Seconds $(if ($n -eq 0) { 8 } else { 1 })
}
foreach ($name in 'REX_MK9_CHECK_LOG', 'REX_MK9_SCRIPT_LOG', 'REX_MK9_NATIVE_LOG', 'REX_MK9_NATIVE_LOG_FRAMES') {
    Remove-Item "env:$name" -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds $Seconds
Stop-Games
for ($n = 0; $n -lt $Players; ++$n) {
    $log = Join-Path $dir "logs\room4_$n.log"
    "== player $($n + 1) ($log)"
    if (Test-Path $log) {
        Select-String -Path $log -Pattern '\[room\]|session started|\[ice\] room .*(answered|offered)|DESYNC|differ from frame|session ended|\[koth\]' |
            Select-Object -First 12 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(260, $_.Line.Length)) }
        Select-String -Path $log -Pattern '\[net\] epoch' | Select-Object -Last 1 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(330, $_.Line.Length)) }
    } else { 'no log' }
}
