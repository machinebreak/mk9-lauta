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

Controles (`mk9_controls.h`, `app/mk9_pc_defaults.toml`): el teclado viene activado con una distribución de pelea
(WASD o flechas = stick izquierdo, U/I/J/K = X/Y/A/B, O = RB agarre, L o Espacio = RT cubrirse, la tecla a la
derecha de la L = LT cambio de guardia, P = LB relevo, Enter o Esc = Start, Tab = Back) y se reasigna en F2 >
Controles. El SDK nombra las teclas por su lugar en un teclado inglés (lee códigos de escaneo): "Semicolon" es la Ñ de
un teclado español. Los mandos van por orden de conexión (el primero es el Jugador 1); el teclado juega con el
jugador de `mk9_keyboard_player` (2: Versus local con teclado y un mando). Online, todos los dispositivos de la PC
manejan al jugador local (`netplay.cpp` ReadLocalPad). Los menús del juego leen el stick, no la cruceta.

Opciones gráficas de Renut (`-Set` o `out\pc\mk9.toml`; para los jugadores, la ventana **Opciones** con F2,
`options_ui.h`: presets Baja / Consola / Alta / Ultra y cada opción en castellano, los controles, pantalla completa o
ventana (también F11 / Alt+Enter) y el idioma del juego (`user_language`: español por defecto, inglés, portugués,
francés, alemán, italiano), guardadas en mk9.toml sin tocar el resto del archivo): `renut_resolution` (480p, 720p, 1080p, 1440p, 4K; cambia en vivo),
`renut_shadow_resolution` (Resolution, 1x-4x), `renut_ambient_occlusion` (Half, Full),
`renut_anisotropic_filtering` (Game, 2x-16x), `renut_post_process_precision` (Game, High), `renut_vsync`,
`sa2_renut_msaa` (0, 2, 4, 8; al reiniciar). `renut_aspect_ratio` no está en este build (widescreen.cpp no se
compila).

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

### PCs con menos núcleos (2026-11-09)

Con el proceso limitado a pocos núcleos (`run_game.ps1 -Affinity`, `pair_test.ps1 -HostAffinity/-GuestAffinity`; el
reloj sigue siendo el del 8600G):

| Núcleos | Prueba | Resultado |
|---|---|---|
| 2 núcleos / 4 hilos (`0F`) | pelea contra la CPU | 60.0 fps, el cuadro más largo 17,6 ms; hilo del juego 2,4 ms por cuadro (lo más 4,2) |
| 1 núcleo / 2 hilos (`03`) | pelea contra la CPU | 60.0 fps; hilo del juego 2,6 ms, hilo de render 1,5 ms, GPU 2,1 ms (720p) |
| 2 núcleos / 4 hilos cada uno | online con rollback, 80 ms + jitter | 60.0 fps los dos; 176 rollbacks (hasta 4 cuadros), guardar un cuadro 1,7 ms, restaurar 1 ms, el cuadro más largo 18,7 ms |

El trabajo por cuadro es chico (código de la 360 corriendo nativo): lo que más pesa en una PC lenta es el reloj de la
CPU con rollbacks hondos (cada cuadro re-simulado cuesta ~2,5 ms acá) y la placa de video (F2 > Calidad "Baja").

### Los textos de la versión de PC (2026-11-09)

La opción "Xbox LIVE" del menú principal dice **ONLINE (F1)** y su mensaje ("tu perfil no tiene permisos para Xbox
LIVE") explica que el online se abre con F1, en los seis idiomas (`mk9_localization.h`, `mk9_pc_texts`). Al arrancar,
los `Coalesced.*` del jugador se reescriben con esos textos en `cache\localization` y esa carpeta se monta sobre
`Localization`: el sistema de archivos del SDK ahora toma el montaje más específico
(`virtual_file_system.cpp`). No se reparten archivos del juego.

### Modos probados (2026-11-09, en español)

Historia (la cinemática de apertura, videos por FFmpeg), Entrenamiento (Tutorial, Práctica), la selección con los 4
personajes de la Komplete (Skarlet, Kenshi, Rain, Freddy Krueger, en la fila extendida de abajo), Torre de los Retos
(el primer reto), la Kripta (menú EXTRAS, carga, entrada, bienvenida), Torneo (Ladder), Versus online. La Kripta va a
30 fps: es el ritmo que el juego pide ahí (el hilo del juego usa ~10 ms y el ritmo espera a 33), no falta de PC.
Los pipelines nuevos aparecen con lo que no se vio antes (la Kripta construyó 82, la Torre 32, la Historia 23, una
pelea ya vista 1): `pipeline_list.bin` los anota y los construye por adelantado en el próximo arranque.

### El arranque de la API gráfica y las notebooks con dos placas (2026-11-11)

`mk9_renderer_start.h`: cada arranque anota en `logs\renderer_start.txt` la API (`d3d12`/`vulkan`), los arranques
fallidos seguidos y cómo terminó (`pending` al crear el dispositivo, `ok` cuando la GPU terminó 120 cuadros, se vean o
no: un juego minimizado no presenta nada, `lost` si se perdió el dispositivo). La segunda vez seguida que una API no
llega a `ok` (el driver se cuelga o se cierra creando el dispositivo, sus primeros pipelines, la swap chain), el
arranque siguiente usa la otra, la guarda en `mk9.toml` y lo dice arriba de la pantalla 20 s. Un dispositivo perdido
lo dice en un mensaje (antes la imagen quedaba congelada) y cuenta como arranque fallido. Probado simulando la segunda
falla: pasa a Vulkan y lo muestra. La ejecutable exporta `NvOptimusEnablement` y `AmdPowerXpressRequestHighPerformance`
(los drivers de NVIDIA y AMD los buscan en el `.exe`; el DLL del SDK los tenía, sin efecto): una notebook con dos
placas usa la dedicada. plume ya elegía, en D3D12, el adaptador con más memoria dedicada y, en Vulkan, la discreta.

## Estado

| Fase | Estado |
|---|---|
| F0 preparación | **hecha**: build de Windows reproducible, el juego corre nativo a 60 fps (Vulkan y D3D12, 720p y 1080p) |
| F1 mapa del motor | avanzado: bucle principal, lectura de mandos, interfaz de captura (Práctica y online), línea de comandos, fenceposts, GMalloc, rand, info de la partida (ver engine-map.md) |
| F2 determinismo | **logrado en lockstep**: dos instancias llegan a la pelea en el mismo cuadro y pelean sin desync (semillas, cargas, películas, "escena lista", barrera; netplay-design.md). Herramientas: grabar/reproducir mandos, hashes, registro de sorteos con pila, de nativas y de scripts |
| F3 online | **hecho**: lobby (WebSocket al lobby compartido), camino propio por ICE (libjuice), salas de dos con chat, partida rápida y chat global, la sesión que se vuelve a abrir ya unida, pantallas online (F1) y fin de sesión con motivo (lobby.md) |
| F4 instantáneas + SyncTest | **núcleo hecho**: línea de tiempo por store cards, SyncTest con 0 diferencias en 744 cuadros repetidos a 60 fps (netplay-design.md, "Rollback") |
| F5 rollback | **anda**: rollback en la pelea con lag y jitter sin desync, cuadros re-simulados sin dibujar, sonido y GPU afuera del rollback, sin sonidos duplicados; tres peleas seguidas con 116 ms de ida y vuelta y 3 % de pérdida sin desync; jugadores con idiomas distintos; el delay de los menús sigue al ping; los avisos de los sonidos al mismo cuadro en las dos (netplay-design.md). Falta: la desincronización rara de los menús, dos PCs reales, TURN |
| F6 pulido | en curso: ventana de Opciones (F2) con presets Baja / Consola / Alta / Ultra, controles (teclado de pelea reasignable, jugador del teclado, mandos conectados), pantalla completa (F11) e idioma (español por defecto); íconos de Xbox y configuración para jugar (`app/mk9.toml`, `app/mk9_pc_defaults.toml`) |
| F7 launcher/beta | empezado: `tools/release/make_release.ps1` arma la carpeta para repartir (exe, runtime de Visual C++, base de mandos, guía, aviso de no afiliación, licencias; sin archivos del juego); al arrancar se verifican los archivos del juego y se avisa en español; si la placa no tiene D3D12 se usa Vulkan. Pendiente: nombre del producto y distribución (decisiones del dueño), lista de pipelines completa, beta |

Para jugar a mano: abrir `out\pc\mk9.exe` (la configuración `out\pc\mk9.toml` ya trae Renut, sonido e íconos de
Xbox). Un mando de Xbox o el teclado.
