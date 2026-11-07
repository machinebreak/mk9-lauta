# MK9 nativo para PC con online rollback (rama `online`)

Fork de [mk9-nx](https://github.com/Thorhax/mk9-nx) (GPL-3.0): el Mortal Kombat Komplete Edition de Xbox 360
recompilado estáticamente a C++ con el SDK ReXGlue. Esta rama le suma lo nuestro: el build de Windows probado, las
herramientas de ingeniería inversa, y (por fases) el online con rollback y las opciones para PCs flojas.

- El plan completo, con fases, riesgos y fuentes: `OneDrive\Desktop\MK9Decomp\PLAN.md`.
- El mapa del motor (F1): [engine-map.md](engine-map.md).
- El remoto `upstream` es mk9-nx; esta rama no se publica en ningún lado. Nada del juego entra al repo: el código
  generado, los archivos del disco, los shaders y los builds están en carpetas ignoradas.

## Carpetas

| Carpeta | Qué hay |
|---|---|
| `C:\Users\Usuario\mk9\mk9-nx` | este repo (fuera de OneDrive: el build tiene decenas de miles de archivos) |
| `C:\Users\Usuario\mk9\_tools\llvm` | LLVM clang 20.1.8 (archivo oficial descomprimido, sin instalador) |
| `C:\Users\Usuario\mk9\_tools\dxc` | DXC 1.9.2609 (release oficial de Microsoft, con SPIR-V) |
| `assets\game_root` | el disco extraído (6,8 GB, 1.265 archivos) |
| `out\` | todo lo generado: `host` (el traductor), `pc` (el juego), shaders, logs |

## Compilar en Windows (probado 2026-11-07)

Requisitos: VS 2022 Build Tools con C++ (trae CMake, Ninja, MSVC y el SDK de Windows), LLVM clang 20 en
`..\_tools\llvm`, Python 3 con `xxhash` y `cryptography` (`python -m pip install --user xxhash cryptography`), Git, y
la ISO propia del MK9 Komplete Edition de Xbox 360 (title `575207FD`, `default.xex` 0.0.0.8, media `6579DAFA`; la
europea `(En,Fr,De,Es,It,Pt)` de 7.838.695.424 bytes sirve).

```powershell
# 1. fuentes de terceros del SDK (una vez)
python tools\fetch_thirdparty.py

# 2. el disco y su verificación (tamaño + SHA-256 de los 1.263 archivos contra app\mk9_game_files.txt)
python tools\extract_iso.py "C:\Users\Usuario\Downloads\Mortal Kombat - Komplete Edition (Europe) (En,Fr,De,Es,It,Pt).iso" -o assets\game_root
python tools\online\verify_game_files.py

# 3. los shaders del juego para Renut: imagen descifrada, escaneo de los paquetes UE3 y los de Direct3D
python tools\xex_image.py assets\game_root\default.xex out\default_image.bin
_tools\llvm\bin\clang++ -std=c++20 -O2 -w shaders\tools\ue3_shader_scan.cpp -o out\tools\ue3_shader_scan.exe
#    (desde assets\game_root\Asset)  ..\..\..\out\tools\ue3_shader_scan.exe <repo>\out\ue3scan *.xxx   -> 19.005
python tools\sa2_dump_shaders.py --image out\default_image.bin --assets assets\game_root --out out\image_shaders   # 87
#    out\renut_shader_dump = out\ue3scan\*.bin + out\image_shaders\*.bin   (19.092)

# 4. DXC para el traductor de shaders de Renut (el repo ignora los binarios)
#    copiar _tools\dxc\bin\x64\* y _tools\dxc\lib\x64\* a app\thirdparty\reblue-XenosRecomp\thirdparty\dxc-bin\{bin,lib}\x64

# 5. todo lo demás: traductor, código del juego (flavour optimised), glslang y el juego
bash tools\online\build_all.sh          # o por pasos: host, gen, glsl, pc
```

Tiempos en esta PC (Ryzen 5 8600G, 31 GB): traductor ~1 min, traducción del juego ~7 min (517 archivos, 274 MB de
C++), glslang 20 s, el juego (1.326 pasos) ver abajo.

Lo que hubo que arreglar para Windows (todo en esta rama):

- `tools/codegen.ps1`, `tools/win/boot_test.ps1`, `tools/win/play.ps1`: todavía nombraban a USF4 (el proyecto del
  que salió mk9-nx). Ahora usan `mk9_manifest.toml`, `out\pc\mk9.exe`, las opciones `mk9_*` y corren
  `post_codegen.py` (el flavour optimised).
- El traductor de shaders de Renut (`app/thirdparty/reblue-XenosRecomp/XenosRecomp/main.cpp`):
  - escribía la caché comprimida (DXIL + SPIR-V de 19.092 shaders) como una lista de números en C++: unos 200
    millones de elementos que Clang guarda como un nodo cada uno (más de 10 GB para ese archivo). Ahora la escribe en
    `shader_cache.cpp.dxil.zst` / `.spirv.zst` y la incluye con `#embed` (`XENOS_RECOMP_BYTE_LISTS=1` vuelve al
    formato viejo);
  - usaba todos los núcleos, con un DXC por hilo (~7 GB con 12 hilos). `XENOS_RECOMP_JOBS=<n>` limita los hilos;
    `build_all.sh` pone 6.

## Enganchar funciones del juego

Como en mk9-nx: `extern "C" REX_FUNC(__imp__sub_X); REX_HOOK_RAW(sub_X) { ...; __imp__sub_X(ctx, base); }` en
`app/src` (ejemplo: `src/mk9_frame_counter.cpp` sobre el Swap `sub_82BF6D58`). Cada `sub_X` generado es un alias
débil de `__imp__sub_X` y el hook lo reemplaza al linkear.

**Ojo:** el paso de llamadas directas del flavour optimised (`tools/direct_calls.py`, dentro de
`post_codegen.py`) cambia `sub_X(ctx, base)` por `__imp__sub_X(ctx, base)` en todas las funciones que no tienen
hook, y para saber cuáles tienen toma cualquier dirección `82xxxxxx` que aparezca en el código de `app/src`, del SDK
y de los toml. Después de agregar un hook nuevo hay que correr
`set REXSDK_DIR=sdk && python tools\direct_calls.py --redo` (o la traducción completa); si no, los llamadores siguen
yendo directo al original y el hook no se ejecuta nunca.

## Correr

`tools\online\run_game.ps1` lanza el juego para una prueba y muestra los `[fps]` y el final del log:

```powershell
# Renut sobre Vulkan, 90 s, minimizado y sin sonido, con capturas a los 30/60/85 s
powershell -File tools\online\run_game.ps1 -Renderer renut -Backend vulkan -Seconds 90 -Snapshots "30,60,85"
# guion de mando hasta una pelea (mk9-notes: título -> menú -> Fight -> Ladder -> Scorpion -> pelea)
powershell -File tools\online\run_game.ps1 -Seconds 150 -Script "32 START;38 A;44 A;52 A;58 A;64 A;70 A;76 A;84 A"
# para jugar a mano: ventana visible y con sonido
powershell -File tools\online\run_game.ps1 -Visible -Sound -Seconds 3600
```

Teclado (sin mando): Enter = Start, Espacio = A, Backspace = B, L = X, P = Y, 3 = RB, E = RT, WASD = stick izquierdo,
Shift + flechas = cruceta.

Opciones gráficas de Renut (`-Set` o `out\pc\mk9.toml`): `renut_resolution` (480p, 720p, 1080p, 1440p, 4K; cambia en
vivo), `renut_shadow_resolution` (Resolution, 1x-4x), `renut_ambient_occlusion` (Half, Full),
`renut_anisotropic_filtering` (Game, 2x-16x), `renut_post_process_precision` (Game, High), `renut_aspect_ratio`
(Auto, 16:9, 21:9, 32:9), `renut_vsync`.

## Estado

| Fase | Estado |
|---|---|
| F0 preparación | en curso: build de Windows hecho hasta los shaders de Renut; falta terminar el build, correrlo y medir |
| F1 mapa del motor | avanzado: bucle principal, lectura de mandos, interfaz de captura, línea de comandos, fenceposts, GMalloc, rand (ver engine-map.md) |
| F2 determinismo | pendiente: grabar/reproducir con el objeto de captura `0x8342D01C`, hash por cuadro |
| F3 online con retraso | pendiente |
| F4 instantáneas + SyncTest | pendiente (el gate) |
| F5 rollback | pendiente |
| F6 pulido, F7 launcher/beta | pendiente |
