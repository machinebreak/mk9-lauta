# Two instances of the PC build on this machine, joined by the online session over local UDP (netplay.cpp): the host
# (player 1) and the guest (player 2). Each gets its own log (out\pc\logs\pair_host.log / pair_guest.log), save folder
# (out\inst_host / out\inst_guest) and, if asked, per-frame state hashes (out\f3\<role>_hash.txt) and the game's own
# per-frame sync checks and random draws (out\f3\<role>_checks.txt, <role>_draws.txt: tools\online\rand_report.py).
#
#   powershell -File tools\online\pair_test.ps1 [-Seconds 110] [-HostScript file] [-GuestScript file] [-Delay 2]
#       [-Snapshots "60,90"] [-Hash] [-HashFrom 1] [-Checks] [-SyncRanges "83380000-834D0000"]
#       [-Room CODE -Lobby ws://127.0.0.1:8791/lobby?game=mk9pc] [-Extra "--name=value ..."] [-Profiles inst]
#       [-HostExtra "--name=value ..."] [-GuestExtra "--name=value ..."] [-HostAffinity 0x0F] [-GuestAffinity 0xF0]
#
# -Room: the two meet in that lobby room and play over the path ICE finds (ice_link.h) instead of local UDP; the lobby
# is a local one by default (ps2-web-recomp's scripts/lobby_server.py 8791 --host 127.0.0.1).
#
# Scripts: mk9_input_script_file format (one "<seconds> <BUTTON> [hold]" per line), each driving its own player.
# Profiles: the save folders are out\<Profiles>_host / _guest (a name never used before gives two fresh profiles).
# Snapshots: the host writes captures\snap_<s>s.ppm at those seconds, the guest at one second later (s+1).
param(
    [int]$Seconds = 110,
    [string]$HostScript = '',
    [string]$GuestScript = '',
    [int]$Delay = 2,
    [int]$Port = 7777,
    [string]$Snapshots = '',
    [switch]$Hash,
    [switch]$Checks,
    [int]$HashFrom = 1,
    [string]$SyncRanges = '',
    [string]$Extra = '',
    [string]$HostExtra = '',
    [string]$GuestExtra = '',
    [string]$HostAffinity = '',
    [string]$GuestAffinity = '',
    [string]$Room = '',
    [string]$Lobby = 'ws://127.0.0.1:8791/lobby?game=mk9pc',
    [string]$Profiles = 'inst',
    [ValidateSet('vulkan', 'd3d12')][string]$Backend = 'd3d12'
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dir = Join-Path $root 'out\pc'
$exe = Join-Path $dir 'mk9.exe'
New-Item -ItemType Directory -Force (Join-Path $root 'out\f3'), (Join-Path $root "out\$($Profiles)_host"), (Join-Path $root "out\$($Profiles)_guest") | Out-Null

Get-CimInstance Win32_Process -Filter "Name='mk9.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -eq $exe } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Remove-Item (Join-Path $dir 'logs\pair_*.log') -ErrorAction SilentlyContinue

function Args-For([string]$role) {
    $a = @('--audio_mute=true', '--window_start_minimized=true', '--sa2_renderer=renut', "--renut_gpu_backend=$Backend",
           "--log_file=logs/pair_$role.log", "--user_data_root=$root\out\$($Profiles)_$role", "--mk9_net_delay=$Delay")
    if ($Room) {
        $name = if ($role -eq 'host') { 'Prueba1' } else { 'Prueba2' }
        $a += "--mk9_net_room=$Room", "--mk9_net_role=$role", "--mk9_lobby_url=$Lobby", "--mk9_online_name=$name"
    } else {
        # direct UDP: no lobby at all (never the real one)
        $a += '--mk9_lobby_url=off'
        if ($role -eq 'host') { $a += "--mk9_net_host=$Port" } else { $a += "--mk9_net_join=127.0.0.1:$Port" }
    }
    $script = if ($role -eq 'host') { $HostScript } else { $GuestScript }
    if ($script) {
        $path = if ([IO.Path]::IsPathRooted($script)) { $script } else { Join-Path $root $script }
        if (-not (Test-Path $path)) { throw "script not found: $path" }
        $a += "--mk9_input_script_file=$path"
    }
    if ($Snapshots) {
        $s = if ($role -eq 'host') { $Snapshots } else { ($Snapshots -split ',' | ForEach-Object { [int]$_ + 1 }) -join ',' }
        $a += "--mk9_snapshot_s=$s"
    }
    if ($Hash) { $a += "--mk9_state_hash=$root\out\f3\$($role)_hash.txt"; $a += "--mk9_state_hash_from=$HashFrom" }
    if ($Checks) {
        $a += "--mk9_check_log=$root\out\f3\$($role)_checks.txt"
        $a += "--mk9_rand_log=$root\out\f3\$($role)_draws.txt"
        $a += "--mk9_native_log=$root\out\f3\$($role)_natives.txt"
        $a += "--mk9_script_log=$root\out\f3\$($role)_scripts.txt"
    }
    if ($SyncRanges) { $a += "--mk9_net_sync_ranges=$SyncRanges" }
    $a += "--mk9_memdump_prefix=$root\out\f3\memdump_$($role)_"
    if ($Extra) { $a += ($Extra -split ' ') }
    $own = if ($role -eq 'host') { $HostExtra } else { $GuestExtra }
    if ($own) { $a += ($own -split ' ') }
    return ($a -join ' ')
}

$started = Get-Date
$h = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru -WindowStyle Minimized -ArgumentList (Args-For 'host')
if ($HostAffinity) { $h.ProcessorAffinity = [IntPtr][Convert]::ToInt64($HostAffinity, 16) }
Start-Sleep -Milliseconds 300
$g = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru -WindowStyle Minimized -ArgumentList (Args-For 'guest')
if ($GuestAffinity) { $g.ProcessorAffinity = [IntPtr][Convert]::ToInt64($GuestAffinity, 16) }
$h.WaitForExit($Seconds * 1000) | Out-Null
foreach ($p in @($h, $g)) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
foreach ($p in @($h, $g)) { $p.WaitForExit(20000) | Out-Null }
# a room session that ended starts the game again by itself (back to the lobby): those go too
Get-CimInstance Win32_Process -Filter "Name='mk9.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -eq $exe } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

foreach ($role in 'host', 'guest') {
    $log = Join-Path $dir "logs\pair_$role.log"
    "== $role ($log)"
    if (Test-Path $log) {
        Select-String -Path $log -Pattern '\[net\]|DESYNC|\[error\]|\[capture\]' | Select-Object -Last 14 | ForEach-Object { $_.Line }
        Select-String -Path $log -Pattern '\[fps\]' | Select-Object -Last 3 | ForEach-Object { $_.Line }
    } else { 'no log' }
}
if ($Snapshots) {
    $python = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($python) { & $python (Join-Path $root 'tools\online\ppm2png.py') (Join-Path $dir 'captures') | Out-Null }
}
