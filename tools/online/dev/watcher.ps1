# Starts a spectator game watching room WATCH1 after a delay, lets it run, then stops it.
param([int]$Delay = 50, [int]$Seconds = 170, [string]$Extra = '', [string]$Snaps = '110,140,165', [string]$Name = 'watch')
Start-Sleep -Seconds $Delay
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$exe = Join-Path $root 'out\pc\mk9.exe'
$a = @('--audio_mute=true', '--window_start_minimized=true', '--sa2_renderer=renut', '--renut_gpu_backend=d3d12',
       "--log_file=logs/$Name.log", "--user_data_root=$root\out\inst_$Name", '--mk9_net_room=WATCH1', '--mk9_net_role=watch',
       '--mk9_lobby_url=ws://127.0.0.1:8791/lobby?game=mk9pc', "--mk9_online_name=M_$Name", '--renut_capture_overlay=true',
       "--mk9_snapshot_s=$Snaps", '--mk9_net_log_ready=true', '--mk9_discord_presence=false',
       "--mk9_check_log=$root\out\f3\$($Name)_checks.txt", "--mk9_rand_log=$root\out\f3\$($Name)_draws.txt",
       "--mk9_native_log=$root\out\f3\$($Name)_natives.txt", "--mk9_script_log=$root\out\f3\$($Name)_scripts.txt",
       "--mk9_sound_log=$root\out\f3\$($Name)_sound.txt")
if ($Extra) { $a += ($Extra -split ' ') }
$p = Start-Process -FilePath $exe -WorkingDirectory (Join-Path $root 'out\pc') -PassThru -WindowStyle Minimized -ArgumentList ($a -join ' ')
"watcher pid $($p.Id)"
$p.WaitForExit($Seconds * 1000) | Out-Null
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
"watcher done"
