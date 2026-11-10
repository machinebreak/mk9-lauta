# The players (room WATCH1, listed while it plays) and a third game that finds it in the lobby's list and presses its
# Watch button (mk9_online_test=watch): the game starts again as the room's spectator.
param([int]$Seconds = 90, [int]$Delay = 12)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$exe = Join-Path $root 'out\pc\mk9.exe'
Remove-Item (Join-Path $root 'out\pc\logs\uiwatch.log') -ErrorAction SilentlyContinue
$pair = Start-Process -FilePath powershell.exe -PassThru -WindowStyle Hidden -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\online\pair_test.ps1'),
    '-Room', 'WATCH1', '-Seconds', "$Seconds", '-HostExtra', '"--mk9_net_room_listed=true --mk9_net_room_title=Sala_de_prueba"')
Start-Sleep -Seconds $Delay
$a = @('--audio_mute=true', '--window_start_minimized=true', '--sa2_renderer=renut', '--renut_gpu_backend=d3d12',
       '--log_file=logs/uiwatch.log', "--user_data_root=$root\out\inst_uiwatch", '--mk9_online_test=watch',
       '--mk9_online_show=true', '--mk9_lobby_url=ws://127.0.0.1:8791/lobby?game=mk9pc', '--mk9_online_name=Mirador2',
       '--renut_capture_overlay=true')
$p = Start-Process -FilePath $exe -WorkingDirectory (Join-Path $root 'out\pc') -PassThru -WindowStyle Minimized -ArgumentList ($a -join ' ')
"third game pid $($p.Id)"
$pair.WaitForExit(($Seconds + 60) * 1000) | Out-Null
"pair done: $($pair.HasExited)"
