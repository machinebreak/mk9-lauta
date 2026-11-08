# Runs the PC build for a test: chosen renderer, scripted gamepad, snapshots, extra game command line; then prints
# the [fps] lines and the end of the log, and converts the snapshots to PNG (tools/online/ppm2png.py).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\online\run_game.ps1 [-Renderer renut|native|xenos]
#       [-Backend vulkan|d3d12] [-Resolution 720p] [-Seconds 60] [-Script "20 START;26 A"] [-ScriptFile path]
#       [-Snapshots "10,20,30"] [-GameArgs "pchr1=scorpion"] [-Visible] [-Sound] [-Set "name=value;name=value"]
#
# -Renderer  renut  = Renut's Direct3D-level renderer (sa2_renderer = renut), on -Backend (renut_gpu_backend)
#            native = the PM4-ring native renderer (sa2_renderer_native; needs sa2_shaders.mesp next to the exe)
#            xenos  = ReXGlue's emulated Xenos GPU (slow; the reference image)
# -GameArgs  appended to the game's own command line (the SDK's cl cvar: "default.xex" <cl>), e.g. pchr1=/bgnd=
# -Set       any other cvars as REX_<NAME> environment variables
# Test runs are muted and minimized unless -Sound / -Visible. The game is stopped after -Seconds.
param(
    [ValidateSet('renut', 'native', 'xenos')][string]$Renderer = 'renut',
    [ValidateSet('vulkan', 'd3d12')][string]$Backend = 'vulkan',
    [string]$Resolution = '',
    [int]$Seconds = 60,
    [string]$Script = '',
    [string]$ScriptFile = '',
    [string]$Snapshots = '',
    [string]$GameArgs = '',
    [string]$Set = '',
    [switch]$Visible,
    [switch]$Sound,
    [int]$Tail = 30,
    [string]$Exe = 'out\pc\mk9.exe'
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$exePath = (Resolve-Path (Join-Path $root $Exe)).Path
$dir = Split-Path $exePath

$env:REX_SA2_RENDERER = if ($Renderer -eq 'renut') { 'renut' } else { 'xenos' }
$env:REX_SA2_RENDERER_NATIVE = if ($Renderer -eq 'native') { 'true' } else { 'false' }
$env:REX_RENUT_GPU_BACKEND = $Backend
if ($Resolution) { $env:REX_RENUT_RESOLUTION = $Resolution }
$env:REX_MK9_SNAPSHOT_S = $Snapshots
$env:REX_MK9_INPUT_SCRIPT = $Script
$env:REX_MK9_INPUT_SCRIPT_FILE = $ScriptFile
$env:REX_CL = $GameArgs
$env:REX_AUDIO_MUTE = if ($Sound) { 'false' } else { 'true' }
$env:REX_WINDOW_START_MINIMIZED = if ($Visible) { 'false' } else { 'true' }
# Tests never reach the real lobby (a shared server): -Set "mk9_lobby_url=ws://127.0.0.1:8791/lobby?game=mk9pc" for a
# local one.
$env:REX_MK9_LOBBY_URL = 'off'
$extra = @()
foreach ($kv in ($Set -split ';')) {
    if ($kv -match '^\s*([A-Za-z0-9_]+)\s*=\s*(.*)$') {
        $name = 'REX_' + $Matches[1].ToUpper()
        Set-Item -Path "env:$name" -Value $Matches[2]
        $extra += $name
    }
}

# A test instance left over from an interrupted run keeps the exe locked: stop instances of this executable only.
Get-CimInstance Win32_Process -Filter "Name='$(Split-Path $exePath -Leaf)'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -eq $exePath } | ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $_.ProcessId -Timeout 20 -ErrorAction SilentlyContinue
    }
if ($Snapshots) { Remove-Item (Join-Path $dir 'captures\*.ppm'), (Join-Path $dir 'captures\*.png') -ErrorAction SilentlyContinue }

$started = Get-Date
$style = if ($Visible) { 'Normal' } else { 'Minimized' }
$p = Start-Process -FilePath $exePath -WorkingDirectory $dir -PassThru -WindowStyle $style
$p.WaitForExit($Seconds * 1000) | Out-Null
if ($p.HasExited) {
    '{0} exited after {1:n1} s with code 0x{2:X8}' -f (Split-Path $exePath -Leaf), ((Get-Date) - $started).TotalSeconds, $p.ExitCode
} else {
    $p.Refresh()
    $status = 'still running after {0} s: cpu {1:n1} s, working set {2} MB; stopped' -f $Seconds, $p.TotalProcessorTime.TotalSeconds, [math]::Round($p.WorkingSet64 / 1MB)
    Stop-Process -Id $p.Id -Force
    if (-not $p.WaitForExit(20000)) { $status += " (warning: process $($p.Id) did not exit within 20 s)" }
    $status
}
foreach ($name in @('REX_SA2_RENDERER', 'REX_SA2_RENDERER_NATIVE', 'REX_RENUT_GPU_BACKEND', 'REX_RENUT_RESOLUTION',
                    'REX_MK9_SNAPSHOT_S', 'REX_MK9_INPUT_SCRIPT', 'REX_MK9_INPUT_SCRIPT_FILE', 'REX_CL',
                    'REX_AUDIO_MUTE', 'REX_WINDOW_START_MINIMIZED') + $extra) {
    Remove-Item "env:$name" -ErrorAction SilentlyContinue
}

$log = Get-ChildItem (Join-Path $dir 'logs') -Filter *.log -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-2) } | Sort-Object LastWriteTime | Select-Object -Last 1
if ($log) {
    "== $($log.FullName) ($($log.Length) bytes)"
    Get-Content $log.FullName -Tail $Tail
    '== [fps]'
    Select-String -Path $log.FullName -Pattern '\[fps\]' | Select-Object -Last 12 | ForEach-Object { $_.Line }
} else {
    'no log written'
}
$caps = Join-Path $dir 'captures'
if ($Snapshots -and (Test-Path $caps)) {
    $python = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($python) { & $python (Join-Path $root 'tools\online\ppm2png.py') $caps }
}
