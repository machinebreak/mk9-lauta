# mk9-lauta (2026-11-17): two to four instances of the PC build on this machine, one session over local UDP
# (netplay.cpp, mk9_net_mesh): machine 0 is the host (player 1), 1-3 the guests (players 2-4), every machine's inputs
# going to every other one. Each gets its own log (out\pc\logs\mesh_<n>.log) and save folder (out\<Profiles>_<n>).
# Nothing reaches a lobby (mk9_lobby_url=off).
#
#   powershell -File tools\online\mesh_test.ps1 [-Machines 4] [-Scripts "a.txt,b.txt,c.txt,d.txt"] [-Seconds 160]
#       [-Port 7400] [-Delay 2] [-Snapshots "120,150"] [-Extra "--name=value ..."]
#       [-MachineExtra "--a=1|--b=2|..."] [-SkipLogos] [-Profiles mesh] [-Backend d3d12] [-Dir out\pc]
#
# Scripts: one file a machine, in its order (mk9_input_script_file format; tools\online\gen_mesh_scripts.py makes the
# four players' of a two against two), timed by frames (mk9_input_script_frames): the session keeps every machine's
# frames in step, so the same frame presses on all of them whatever the PC's load. -MachineExtra: options for each
# machine, '|'-separated in its order (one machine's network made worse: --mk9_net_lag_ms=60). Snapshots: machine n
# writes captures\snap_<s+n>s.ppm.
param(
    [ValidateRange(2, 4)][int]$Machines = 4,
    [string]$Scripts = '',
    [int]$Seconds = 160,
    [int]$Port = 7400,
    [int]$Delay = 2,
    [string]$Snapshots = '',
    [string]$Extra = '',
    [string]$MachineExtra = '',
    [switch]$SkipLogos,
    [string]$Profiles = 'mesh',
    [ValidateSet('vulkan', 'd3d12', 'd3d11')][string]$Backend = 'd3d12',
    [string]$Dir = 'out\pc'
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$dir = if ([IO.Path]::IsPathRooted($Dir)) { $Dir } else { Join-Path $root $Dir }
$exe = Join-Path $dir 'mk9.exe'
$scriptList = if ($Scripts) { $Scripts -split ',' } else { @() }
$machineExtras = if ($MachineExtra) { $MachineExtra -split '\|' } else { @() }

Get-CimInstance Win32_Process -Filter "Name='mk9.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -eq $exe } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Remove-Item (Join-Path $dir 'logs\mesh_*.log') -ErrorAction SilentlyContinue

function Args-For([int]$n) {
    New-Item -ItemType Directory -Force (Join-Path $root "out\$($Profiles)_$n") | Out-Null
    $a = @('--audio_mute=true', '--window_start_minimized=true', '--sa2_renderer=renut', "--renut_gpu_backend=$Backend",
           "--log_file=logs/mesh_$n.log", "--user_data_root=$root\out\$($Profiles)_$n", "--mk9_net_delay=$Delay",
           "--mk9_net_skip_logos=$(if ($SkipLogos) { 'true' } else { 'false' })", '--mk9_discord_presence=false',
           '--mk9_lobby_url=off', "--mk9_net_mesh=$Port", "--mk9_net_machines=$Machines", "--mk9_net_machine=$n",
           '--mk9_input_script_frames=true')
    if ($n -lt $scriptList.Count -and $scriptList[$n]) {
        $path = if ([IO.Path]::IsPathRooted($scriptList[$n])) { $scriptList[$n] } else { Join-Path $root $scriptList[$n] }
        if (-not (Test-Path $path)) { throw "script not found: $path" }
        $a += "--mk9_input_script_file=$path"
    }
    if ($Snapshots) { $a += "--mk9_snapshot_s=" + (($Snapshots -split ',' | ForEach-Object { [int]$_ + $n }) -join ',') }
    if ($Extra) { $a += ($Extra -split ' ') }
    if ($n -lt $machineExtras.Count -and $machineExtras[$n]) { $a += ($machineExtras[$n] -split ' ') }
    return ($a -join ' ')
}

$procs = @()
for ($n = 0; $n -lt $Machines; ++$n) {
    $procs += Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru -WindowStyle Minimized -ArgumentList (Args-For $n)
    Start-Sleep -Milliseconds 300
}
$procs[0].WaitForExit($Seconds * 1000) | Out-Null
foreach ($p in $procs) { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
foreach ($p in $procs) { $p.WaitForExit(20000) | Out-Null }

for ($n = 0; $n -lt $Machines; ++$n) {
    $log = Join-Path $dir "logs\mesh_$n.log"
    "== machine $n (player $($n + 1)) ($log)"
    if (Test-Path $log) {
        Select-String -Path $log -Pattern 'session started|DESYNC|differ from frame|session ended|\[error\]' |
            Select-Object -First 8 | ForEach-Object { $_.Line }
        Select-String -Path $log -Pattern '\[net\] epoch' | Select-Object -Last 1 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(420, $_.Line.Length)) }
        Select-String -Path $log -Pattern '\[fps\]' | Select-Object -Last 2 | ForEach-Object { $_.Line }
    } else { 'no log' }
}
if ($Snapshots) {
    $python = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($python) { & $python (Join-Path $root 'tools\online\ppm2png.py') (Join-Path $dir 'captures') | Out-Null }
}
