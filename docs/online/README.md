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
C++), glslang 20 s; el juego: los 255 archivos generados ~4 s cada uno (~15 min con 3-4 en paralelo), la traducción
de los 19.092 shaders de Renut ~30 min con 6 hilos (sólo la primera vez o si cambia el traductor), el enlace ~1 min.
`out\pc\mk9.exe` pesa 120 MB (+146 MB de símbolos en `mk9.pdb`).

**Ojo con el traductor de shaders:** depende del ejecutable de XenosRecomp, que se recompila cada vez que cambia su
código (`BUILD_ALWAYS`): un cambio en `reblue-XenosRecomp` vuelve a traducir los 19.092 shaders (~30 min). Si sólo
fue un comentario: `touch out/pc/renut_gpu/shader_cache.cpp*` y `ninja -C out/pc -t restat` antes de compilar
(ninja compara la fecha que anotó en su registro, no la del archivo).

Lo que hubo que arreglar para Windows (todo en esta rama):

- `tools/codegen.ps1`, `tools/win/boot_test.ps1`, `tools/win/play.ps1`: todavía nombraban a USF4 (el proyecto del
  que salió mk9-nx). Ahora usan `mk9_manifest.toml`, `out\pc\mk9.exe`, las opciones `mk9_*` y corren
  `post_codegen.py` (el flavour optimised).
- El traductor de shaders de Renut (`app/thirdparty/reblue-XenosRecomp/XenosRecomp/main.cpp`):
  - escribía la caché comprimida (DXIL 16,6 MB + SPIR-V 14 MB de 19.092 shaders) como una lista de números en C++:
    110 MB de texto y 30 millones de elementos, que Clang guarda como un nodo cada uno (gigas de RAM para ese
    archivo). Ahora la escribe en `shader_cache.cpp.dxil.zst` / `.spirv.zst` y la incluye con `#embed`
    (`XENOS_RECOMP_BYTE_LISTS=1` vuelve al formato viejo);
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

## Medido (2026-11-07, Ryzen 5 8600G + Radeon 760M integrada)

Del arranque a una pelea del Arcade Ladder con el guion de arriba: logos, menús, selección, pelea con HUD, todo bien
dibujado. Sin errores en el log fuera de los esperados (`UPDATE:` no existe: no hay actualización de título).

| Renderer | Resolución | Pelea | Por cuadro |
|---|---|---|---|
| Renut + Vulkan | 720p | 60 fps | juego 3 ms, hilo de render 2,3 ms, GPU ~2,2 ms por envío (2 por cuadro) |
| Renut + D3D12 | 720p | 60 fps | hilo de render 1,7-2,4 ms, GPU ~4,4 ms |
| Renut + Vulkan | 1080p | 60 fps | GPU ~4,4 ms por envío (~9 ms por cuadro) |

Latencia medida por el propio renderer: mando -> pantalla ~24 ms. Tirones: algunos cuadros de 0,3-1 s en cargas y
en el primer uso de pipelines nuevos. Dos shaders que faltaban quedaron en `out\pc\shader_capture` y el próximo
build los suma solo.

## Estado

| Fase | Estado |
|---|---|
| F0 preparación | **hecha**: build de Windows reproducible, el juego corre nativo a 60 fps (Vulkan y D3D12, 720p y 1080p) |
| F1 mapa del motor | avanzado: bucle principal, lectura de mandos, interfaz de captura (Práctica y online), línea de comandos, fenceposts, GMalloc, rand, info de la partida (ver engine-map.md) |
| F2 determinismo | herramientas hechas y probadas: `mk9_input_record` / `mk9_input_replay` (una corrida reproducida sin guion llega sola a la pelea), `mk9_state_hash` (+`_pages`). Falta: fijar la semilla (el rival del Ladder cambia entre arranques) y encontrar las páginas deterministas |
| F3 online con retraso | diseñado (netplay-design.md, camino A); pendiente |
| F4 instantáneas + SyncTest | pendiente (el gate) |
| F5 rollback | pendiente |
| F6 pulido, F7 launcher/beta | pendiente; ya están los íconos de Xbox y la configuración para jugar (`app/mk9.toml`) |

Para jugar a mano: abrir `out\pc\mk9.exe` (la configuración `out\pc\mk9.toml` ya trae Renut, sonido e íconos de
Xbox). Un mando de Xbox o el teclado.
