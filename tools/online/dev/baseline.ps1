# The online plan's baseline (docs/online/baseline-0.3.0.md): an offline fight against the CPU, online fights at three
# round trips, one with each game on two cores, each game's memory in the menus and in the fight. The logs go to
# out\f3\baseline\<scenario>_<role>.log; tools\online\dev\baseline_report.py reads them.
#
#   powershell -File tools\online\dev\baseline.ps1 [-Only "offline,lag25,lag45,lag70,slow45"] [-Seconds 200]
param([string]$Only = 'offline,lag25,lag45,lag70,slow45', [int]$Seconds = 200, [string]$Dir = 'out\pc')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$dir = if ([IO.Path]::IsPathRooted($Dir)) { $Dir } else { Join-Path $root $Dir }
$exe = Join-Path $dir 'mk9.exe'
$out = Join-Path $root 'out\f3\baseline'
New-Item -ItemType Directory -Force $out | Out-Null
$memory = Join-Path $out 'memory.txt'

function Sample([string]$scenario, [string]$when) {
    Get-CimInstance Win32_Process -Filter "Name='mk9.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.ExecutablePath -eq $exe } | ForEach-Object {
            $p = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
            if ($p) {
                $role = if ($_.CommandLine -match 'pair_(host|guest)') { $Matches[1] } else { 'offline' }
                Add-Content -Path $memory -Value ("{0} {1} {2} working {3:n0} MB private {4:n0} MB" -f $scenario,
                    $role, $when, ($p.WorkingSet64 / 1MB), ($p.PrivateMemorySize64 / 1MB))
            }
        }
}

function Online([string]$name, [int]$lag, [string]$hostAffinity = '', [string]$guestAffinity = '') {
    $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root 'tools\online\pair_test.ps1'),
              '-Seconds', "$Seconds", '-HostScript', 'out\f3\host_long.txt', '-GuestScript', 'out\f3\guest_long.txt',
              '-Extra', "`"--mk9_net_lag_ms=$lag --mk9_net_jitter_ms=10`"", '-Dir', $Dir)
    if ($hostAffinity) { $args += '-HostAffinity', $hostAffinity, '-GuestAffinity', $guestAffinity }
    $pair = Start-Process -FilePath powershell.exe -PassThru -WindowStyle Hidden -ArgumentList $args
    Start-Sleep -Seconds 45
    Sample $name 'menus'
    Start-Sleep -Seconds 100
    Sample $name 'fight'
    $pair.WaitForExit(($Seconds + 90) * 1000) | Out-Null
    foreach ($role in 'host', 'guest') {
        Copy-Item (Join-Path $dir "logs\pair_$role.log") (Join-Path $out "$($name)_$role.log") -Force
    }
    "$name done"
}

function Offline() {
    $log = 'base_offline'
    Remove-Item (Join-Path $dir "logs\$log.log") -ErrorAction SilentlyContinue
    $a = @('--audio_mute=true', '--window_start_minimized=true', '--sa2_renderer=renut', '--renut_gpu_backend=d3d12',
           "--log_file=logs/$log.log", "--user_data_root=$root\out\inst_base", '--mk9_lobby_url=off',
           '--mk9_discord_presence=false', '"--mk9_input_script=32 START;38 A;44 A;52 A;58 A;64 A;70 A;76 A;84 A"',
           '--mk9_snapshot_s=140')
    $p = Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru -WindowStyle Minimized -ArgumentList ($a -join ' ')
    Start-Sleep -Seconds 45
    Sample 'offline' 'menus'
    Start-Sleep -Seconds 90
    Sample 'offline' 'fight'
    $p.WaitForExit(15000) | Out-Null
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    Copy-Item (Join-Path $dir "logs\$log.log") (Join-Path $out 'offline_offline.log') -Force
    'offline done'
}

foreach ($s in ($Only -split ',')) {
    switch ($s.Trim()) {
        'offline' { Offline }
        'lag25' { Online 'lag25' 25 }
        'lag45' { Online 'lag45' 45 }
        'lag70' { Online 'lag70' 70 }
        'slow45' { Online 'slow45' 45 '0x05' '0x50' }
    }
}
