# The PC release's folder, ready to zip: the game's executable and what it needs to run on a PC that has nothing
# installed, the players' guide and the licences. No game files: each player copies the game_root of their own copy.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\release\make_release.ps1 [-Out out\release\mk9pc] [-Zip]
#
# What goes in:
#   mk9.exe, rexruntimerd.dll, rexgpu-xenosrd.dll       the build (out\pc)
#   gamecontrollerdb.txt                                SDL's community controller mappings (zlib)
#   mk9.toml                                            the players' settings template (app\mk9.toml)
#   pipeline_list.bin                                   the GPU pipelines built while the game starts, so the first
#                                                       fights do not stutter (out\pc's, from the test runs)
#   msvcp140*.dll, vcruntime140*.dll                    Visual C++'s runtime beside the executable (Microsoft lets it
#                                                       be deployed so): no installer needed
#   LEEME.txt, AVISO.txt, licencias\                    the guide (docs\online\guia-jugadores.md), the notice, every
#                                                       licence of the code in the executable
#   game_root\COPIAR_ACA.txt                            where the game files go
# Nothing is published or uploaded: the folder (and the zip with -Zip) stays on this PC.
param(
    [string]$Out = 'out\release\mk9pc',
    [switch]$Zip
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$pc = Join-Path $root 'out\pc'
$dst = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }

# Links inside the old folder (a tester's game_root linked to the real game files) go first, as links: Windows
# PowerShell's Remove-Item -Recurse would go through a junction and delete what it points to.
function Remove-Links([string]$dir) {
    foreach ($item in Get-ChildItem -LiteralPath $dir -Force -ErrorAction SilentlyContinue) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            if ($item.PSIsContainer) { [IO.Directory]::Delete($item.FullName) } else { [IO.File]::Delete($item.FullName) }
        } elseif ($item.PSIsContainer) {
            Remove-Links $item.FullName
        }
    }
}
if (Test-Path $dst) {
    Remove-Links $dst
    Remove-Item -Recurse -Force $dst
}
New-Item -ItemType Directory -Force $dst, (Join-Path $dst 'licencias'), (Join-Path $dst 'game_root') | Out-Null

# --- the build ---
foreach ($f in 'mk9.exe', 'rexruntimerd.dll', 'rexgpu-xenosrd.dll', 'gamecontrollerdb.txt') {
    $src = Join-Path $pc $f
    if (-not (Test-Path $src)) { throw "missing from the build: $src" }
    Copy-Item $src $dst
}
Copy-Item (Join-Path $root 'app\mk9.toml') $dst
$pipelines = Join-Path $pc 'pipeline_list.bin'
if (Test-Path $pipelines) {
    Copy-Item $pipelines $dst
} else {
    Write-Warning 'no pipeline_list.bin in out\pc: the first fights will build their pipelines as they go (stutter)'
}

# --- Visual C++'s runtime, from the newest redistributable folder of the build tools ---
$vs = @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools", "$env:ProgramFiles\Microsoft Visual Studio\2022\Community",
        "$env:ProgramFiles\Microsoft Visual Studio\2022\BuildTools") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vs) { throw 'Visual Studio 2022 (its VC\Redist folder) not found' }
$crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\Microsoft.VC143.CRT' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $crt) { throw 'Microsoft.VC143.CRT not found under VC\Redist\MSVC' }
foreach ($f in 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll',
               'vcruntime140_1.dll') {
    Copy-Item (Join-Path $crt $f) $dst
}

# --- the guide, the notice, the game files' place ---
$guide = Get-Content -Raw -Encoding UTF8 (Join-Path $root 'docs\online\guia-jugadores.md')
$install = @"
INSTALACIÓN
===========

1. Descomprimí esta carpeta donde quieras (no adentro de "Archivos de programa").
2. Copiá la carpeta game_root de tu copia del juego (Xbox 360, Komplete Edition: default.xex y las carpetas Asset,
   Localization, Movies y las demás) adentro de la carpeta game_root de acá, reemplazando el archivo COPIAR_ACA.txt.
3. Abrí mk9.exe. La primera vez tarda un poco más: prepara los gráficos de tu placa de video.
   - Si Windows muestra "Windows protegió tu PC" (el programa no tiene firma digital): "Más información" y
     "Ejecutar de todas formas".
   - La primera vez que entrás al online, Windows pregunta si dejás que el juego use la red: tocá "Permitir"
     (marcá redes privadas y públicas). Sin eso no se puede conectar con el rival.

Requisitos: Windows 10 u 11 de 64 bits, una placa de video con DirectX 12 o Vulkan (casi todas desde 2013) y un
procesador de 4 núcleos. Para PCs con poca placa de video: F2 > Gráficos > Calidad "Baja".

Si algo falla, la carpeta logs tiene lo que pasó: mandá el último archivo mk9_*.log (y, si el juego se cerró de
golpe, el crash_*.dmp de la misma hora).


"@
[IO.File]::WriteAllText((Join-Path $dst 'LEEME.txt'), ($install + $guide).Replace("`r`n", "`n").Replace("`n", "`r`n"),
    [Text.UTF8Encoding]::new($true))

$notice = @"
AVISO

Este programa es un trabajo de fans, gratuito y sin fines de lucro. No está afiliado, asociado, autorizado ni
respaldado por Warner Bros. Games, NetherRealm Studios, Microsoft ni ningún otro dueño de los derechos del juego
original. No incluye ningún archivo del juego: hace falta una copia propia y legítima del juego original de Xbox 360.
Todas las marcas pertenecen a sus dueños.

El código de este programa se distribuye bajo la licencia GNU GPL versión 3 (licencias\mk9-nx GPL-3.0.txt); las
bibliotecas de terceros, bajo las suyas (carpeta licencias).
"@
[IO.File]::WriteAllText((Join-Path $dst 'AVISO.txt'), $notice.Replace("`r`n", "`n").Replace("`n", "`r`n"),
    [Text.UTF8Encoding]::new($true))
[IO.File]::WriteAllText((Join-Path $dst 'game_root\COPIAR_ACA.txt'),
    "Copiá acá los archivos de tu copia del juego (default.xex y las carpetas Asset, Localization, Movies...).`r`n",
    [Text.UTF8Encoding]::new($true))

# --- licences: this project's, the SDK's, every third-party component's, the controller database's, Visual C++'s ---
$lic = Join-Path $dst 'licencias'
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $lic 'mk9-nx GPL-3.0.txt')
Copy-Item (Join-Path $root 'sdk\LICENSE') (Join-Path $lic 'ReXGlue SDK.txt')
Copy-Item (Join-Path $root 'app\gamecontrollerdb.LICENSE.txt') (Join-Path $lic 'SDL_GameControllerDB.txt')
foreach ($dir in (Get-ChildItem (Join-Path $root 'app\thirdparty') -Directory) + (Get-ChildItem (Join-Path $root 'sdk\thirdparty') -Directory)) {
    $files = Get-ChildItem $dir.FullName -File | Where-Object { $_.Name -match '^(LICENSE|LICENCE|COPYING|COPYRIGHT|NOTICE)' }
    foreach ($f in $files) { Copy-Item $f.FullName (Join-Path $lic ("{0} - {1}.txt" -f $dir.Name, $f.BaseName)) }
}
[IO.File]::WriteAllText((Join-Path $lic 'Microsoft Visual C++ Runtime.txt'),
    "msvcp140*.dll y vcruntime140*.dll: Microsoft Visual C++ 2015-2022 Redistributable, distribuidos junto al " +
    "programa como lo permiten los términos de licencia de Microsoft Visual Studio (Distributable Code).`r`n",
    [Text.UTF8Encoding]::new($true))

$size = (Get-ChildItem $dst -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
'{0}: {1} files, {2:n1} MB' -f $dst, (Get-ChildItem $dst -Recurse -File).Count, $size
if ($Zip) {
    $zipPath = "$dst.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath }
    Compress-Archive -Path (Join-Path $dst '*') -DestinationPath $zipPath -CompressionLevel Optimal
    '{0}: {1:n1} MB' -f $zipPath, ((Get-Item $zipPath).Length / 1MB)
}
