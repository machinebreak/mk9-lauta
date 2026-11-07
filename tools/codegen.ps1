# Translates the game (default.xex) to C++ with the host code generator (Windows counterpart of tools/codegen.sh). It
# builds nothing: build the generator first
# (tools\win\run.ps1 out\host_build.log "cmake --build out/host --target rexglue").
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\codegen.ps1 [-Flavor classic|optimised] [-Rexglue path]
#
# -Flavor (default: keep what app\codegen_flavor.toml says): optimised copies app\perf_overrides.toml to
# app\codegen_flavor.toml (registers as locals, no MSR lock); classic empties it. Then tools\post_codegen.py runs the
# flavour's post-codegen steps (no-volatile pch + loop barriers + direct calls for optimised), as codegen.sh does; the
# build appends the same script to its own codegen command (app/CMakeLists.txt).
#
# Input:   assets\game_root\default.xex  (tools\extract_iso.py puts it there)
#          app\mk9_manifest.toml          (project, game paths, includes: overrides.toml, codegen_flavor.toml, jobs.toml)
# Output:  app\generated\                 (C++ of the game and generated\rexglue.cmake; not committed)
#          out\codegen.log                (log of the generator)
param(
    [ValidateSet('', 'classic', 'optimised')][string]$Flavor = '',
    [string]$Rexglue = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$app = Join-Path $root 'app'
$flavorToml = Join-Path $app 'codegen_flavor.toml'
switch ($Flavor) {
    'optimised' {
        $text = "# optimised codegen: written by tools/codegen.sh --flavor optimised from perf_overrides.toml.`n" +
                [IO.File]::ReadAllText((Join-Path $app 'perf_overrides.toml'))
        [IO.File]::WriteAllText($flavorToml, $text.Replace("`r`n", "`n"))
    }
    'classic' {
        [IO.File]::WriteAllText($flavorToml, "# classic codegen: no extra options (tools/codegen.sh --flavor classic writes this file; --flavor optimised`n# copies perf_overrides.toml here).`n")
    }
}
if (-not $Rexglue) {
    # The newest generator: rexglue.exe (host build) or rexgluerd.exe (the PC tree's RelWithDebInfo build).
    $Rexglue = Get-ChildItem (Join-Path $root 'sdk\out') -Recurse -Include rexglue.exe, rexgluerd.exe -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Rexglue -or -not (Test-Path $Rexglue)) { throw 'rexglue.exe not found; build it first (see the header)' }
$xex = Join-Path $root 'assets\game_root\default.xex'
if (-not (Test-Path $xex)) { throw "$xex not found; extract your disc with tools\extract_iso.py first" }

New-Item -ItemType Directory -Force (Join-Path $root 'out') | Out-Null
$log = Join-Path $root 'out\codegen.log'
Write-Host "== codegen with $Rexglue"
Push-Location $app
try {
    $env:REX_MAX_JUMP_TABLE_ENTRIES = '1024'
    & $env:ComSpec /c "`"$Rexglue`" codegen mk9_manifest.toml > `"$log`" 2>&1"
    $code = $LASTEXITCODE
} finally { Pop-Location }
Get-Content $log -Tail 30
if ($code -ne 0) { throw "codegen failed (exit $code), see out\codegen.log" }

Write-Host '== post-codegen steps (tools\post_codegen.py)'
$env:REXSDK_DIR = Join-Path $root 'sdk'
$python = (Get-Command python -ErrorAction Stop).Source
& $python (Join-Path $root 'tools\post_codegen.py')
if ($LASTEXITCODE -ne 0) { throw "post_codegen.py failed (exit $LASTEXITCODE)" }
Write-Host "done: $app\generated"
