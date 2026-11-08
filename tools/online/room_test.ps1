# The online flow end to end on this machine: two games meet in a lobby room, both say they are ready, the host starts,
# both games start again already joined (session.h) and play a fight over the path ICE finds. The host opens a public
# room (mk9_online_test=host), the guest uses quick match (=quick). A local lobby is expected (ps2-web-recomp's
# scripts/lobby_server.py 8791 --host 127.0.0.1). Logs: out\pc\logs\room_host.log / room_guest.log (each game's, the
# one started again appends to it).
#
#   powershell -File tools\online\room_test.ps1 [-Seconds 160] [-Lobby ws://127.0.0.1:8791/lobby?game=mk9pc]
#       [-HostScript out\f3\host_fight.txt] [-GuestScript out\f3\guest_fight.txt] [-Extra "--name=value ..."]
param(
    [int]$Seconds = 160,
    [string]$Lobby = 'ws://127.0.0.1:8791/lobby?game=mk9pc',
    [string]$HostScript = 'out\f3\host_fight.txt',
    [string]$GuestScript = 'out\f3\guest_fight.txt',
    [string]$Extra = '',
    [string]$HostExtra = ''
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dir = Join-Path $root 'out\pc'
$exe = Join-Path $dir 'mk9.exe'
function Stop-Games {
    Get-CimInstance Win32_Process -Filter "Name='mk9.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -eq $exe } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
Stop-Games
Remove-Item (Join-Path $dir 'logs\room_*.log') -ErrorAction SilentlyContinue
function Args-For([string]$role) {
    $script = if ($role -eq 'host') { $HostScript } else { $GuestScript }
    $a = @('--audio_mute=true', '--window_start_minimized=true', '--sa2_renderer=renut', '--renut_gpu_backend=d3d12',
           "--log_file=logs/room_$role.log", "--mk9_lobby_url=$Lobby",
           "--mk9_online_test=$(if ($role -eq 'host') { 'host' } else { 'quick' })",
           "--mk9_online_name=$(if ($role -eq 'host') { 'Prueba1' } else { 'Prueba2' })",
           "--mk9_input_script_file=$(Join-Path $root $script)")
    if ($Extra) { $a += ($Extra -split ' ') }
    if ($role -eq 'host' -and $HostExtra) { $a += ($HostExtra -split ' ') }
    return ($a -join ' ')
}
Start-Process -FilePath $exe -WorkingDirectory $dir -WindowStyle Minimized -ArgumentList (Args-For 'host') | Out-Null
Start-Sleep -Milliseconds 1500
Start-Process -FilePath $exe -WorkingDirectory $dir -WindowStyle Minimized -ArgumentList (Args-For 'guest') | Out-Null
Start-Sleep -Seconds $Seconds
Stop-Games
foreach ($role in 'host', 'guest') {
    $log = Join-Path $dir "logs\room_$role.log"
    "== $role"
    if (Test-Path $log) {
        Select-String -Path $log -Pattern '\[room\]|\[session\]|\[ice\] room|session started|DESYNC|rollback on|epoch 1 frame' |
            Select-Object -Last 16 | ForEach-Object { $_.Line.Substring([Math]::Min(40, $_.Line.Length)) }
    } else { 'no log' }
}
