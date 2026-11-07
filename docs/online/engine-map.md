# Mapa del motor para el online (F1)

Notas de ingeniería inversa del MK9 Komplete Edition de Xbox 360 (`default.xex` 0.0.0.8, title `575207FD`, media
`6579DAFA`) para el online con rollback de la rama `online`. Las direcciones son del juego (guest), sobre la imagen
cargada `out/default_image.bin` (base `0x82000000`, la escribe `tools/xex_image.py`). Cada dato dice de dónde sale.

Herramientas (`tools/online/`):

- `image_strings.py`: los textos de la imagen con su dirección (ASCII y UTF-16BE).
  `python tools/online/image_strings.py > out/image_strings.txt`
- `ppc_xref.py`: qué código arma una dirección (propagación de constantes lis/addi/ori/cargas en cada función de
  `.pdata`), qué datos la contienen (`--range` para un bloque), qué función contiene una instrucción (`--func`) y
  quién llama a una función (`--calls`). `python tools/online/ppc_xref.py 821724E8`, `... "GameSpeed"`,
  `... --calls 829D5230`.
- El C++ generado (`app/generated/default/mk9_recomp.*.cpp`) trae cada instrucción PPC como comentario: sirve de
  listado (`grep -n "DEFINE_REX_FUNC(sub_829D5230)" app/generated/default/*.cpp`). El LLVM de Windows no trae el
  desensamblador de PowerPC.

## Secciones de la imagen

| Sección | Desde | Tamaño |
|---|---|---|
| `.rdata` | `0x82000400` | `0x2B3004` |
| `.pdata` | `0x822B3600` | `0xA2500` (las funciones: inicio + largo) |
| `.text` | `0x82360000` | `0x1016864` |
| `.data` | `0x83380000` | `0x14FCE0` |
| `.idata` | `0x834D0000` | `0x54C` |
| `.reloc` | `0x834E0200` | `0x14D020` (todas las palabras absolutas: vtables, tablas de punteros) |

Los thunks de importación están al final de `.text` (`0x8337xxxx`): dos palabras `0x01LLOOOO`/`0x02LLOOOO` (librería
`LL`: 00 = xam, 01 = xboxkrnl; ordinal `OOOO`), `mtctr r11`, `bctr`.

## El cuadro: bucle principal

| Qué | Dónde | Evidencia |
|---|---|---|
| Punto de entrada del XEX | `sub_8236E580` | cabecera del XEX (`extract_iso.py --info`) |
| `main` (crea el motor y entra al bucle) | `sub_823669D8` | llamado desde el punto de entrada |
| `GuardedMain`: el bucle de cuadros | `sub_823667E8` | ver abajo |
| `GEngineLoop` (objeto `FEngineLoop`) | `0x833C6290` | `addi r24,r11,25232` con `lis -31940`, se pasa a PreInit/Init/Tick/Exit |
| `FEngineLoop::PreInit` (copia la línea de comandos) | `sub_82365A40` | primera llamada del bucle; escribe el búfer de `appCmdLine` |
| `FEngineLoop::Init` | `sub_82364A20` | segunda llamada |
| `FEngineLoop::Tick` (un cuadro del motor) | `sub_823621F8` | llamado una vez por vuelta; adentro, la espera al hilo de render `sub_82741630` (mk9-notes) |
| `FEngineLoop::Exit` | `sub_823632D8` | después del bucle |
| `GIsRequestingExit` | `0x833DD0D8` | el bucle sigue mientras vale 0 |
| "leer mandos" habilitado | `0x833DD534` | si no es 0 el bucle llama a la lectura de mandos |
| Duración del último cuadro | `0x833DD55C` | diferencia de dos lecturas del reloj (`sub_82366BF8`) |
| Contadores de cuadros | `[0x833848D4]+108`, `+112` | se incrementan en cada vuelta |

Cada vuelta de `sub_823667E8`: reloj, contador de cuadros, **lectura de mandos** (`sub_829D5230` y `sub_829D7748`, sólo
si `0x833DD534 != 0`), `sub_8239D550`, `sub_829D49F0`, **`FEngineLoop::Tick`**, reloj, mensajes del sistema
(`sub_82360F10`), y vuelve a empezar mientras `GIsRequestingExit == 0`. Hay una sola lectura de mandos por cuadro y
corre en el hilo del juego antes del tick: es el límite de cuadro natural para el netcode (como el `T6InputHook` de
Tekken 6 Native Online).

## Entrada

| Qué | Dónde | Evidencia |
|---|---|---|
| Thunk `XamInputGetCapabilities` / `XamInputGetState` / `XamInputSetState` | `0x83375604` / `0x83375614` / `0x83375624` | registros de importación `0x01000190..92` |
| `XInputGetState(user, state)` = `XamInputGetState(user, 1, state)` | `0x82366C30` | `mr r5,r4; li r4,1; b 0x83375614` (sin `.pdata`) |
| `XInputGetCapabilities` | `0x82366C28` | `b 0x83375604` |
| Lectura de los 4 mandos por cuadro | `sub_829D5230` | llama a `XInputGetState` por usuario; estados crudos desde `0x83425368`, bloques por usuario desde `0x83426F70` |
| Segunda lectura (otros dispositivos) | `sub_829D7748` -> `sub_829D7230` | también llama a `XInputGetState` |

**Interfaz de captura de entradas** (la grabación de la Práctica y los mandos del online):

- `sub_82A1C550(tag, data, size, user)` llama al método `+4` de un objeto global en `0x8342D01C` si hay uno. La
  lectura de mandos la llama con `0x00FF0003` (resultado de `XInputGetState`, 4 bytes), `0x00FF0002` (el estado crudo,
  16 bytes) y `0x00FF0001` (12 bytes procesados), por usuario, y después vuelve a leer el bloque: el objeto puede
  reemplazarlo. 32 lugares la llaman (`--calls 82A1C550`).
- vtable del objeto: `+0` destructor, `+4` capturar bloque, `+8` capturar valor (`sub_82A1C5C0`), `+20` cerrar,
  `+28` ¿grabando? (`sub_82A1C700`), `+32` ¿reproduciendo? (`sub_82A1C750`); `sub_82A1C640` lo destruye y pone 0.
- Lo instala `sub_82A1C6D8` (`lis r11,-31933; stw r3,-12260(r11); blr`, hoja sin `.pdata`: `ppc_xref` no ve su
  referencia, sí la llamada). La llaman tres lugares:
  - `sub_823D98D8`: el **modo Práctica**. La zona `0x823D8000-0x823DC000` es la Práctica (textos `ui_i_recorder`,
    `RecorderStop`, `PlaybackStop`, `player.record.SetProgress`, `ResetForPractice`, `practice_reset_camera`): el
    objeto es el grabador que graba y reproduce al muñeco.
  - `sub_823DB1C0`: la selección de personaje (`PlayerSelect`, `%s/%s vs %s/%s %s`).
  - `sub_824088A0`: **el online** (MKOnline, al lado de `MKOnline_SetupGamepads` y del volcado de
    `m_LocalGamepads`/`m_RemoteGamepads`). Reserva un objeto de 12 bytes con GMalloc y lo instala: el online del
    juego mete los mandos del rival reemplazando los bloques del jugador remoto en la lectura de cada cuadro.
- Para nosotros: el hook nativo de `sub_82A1C550` (F2, `app/src/online/mk9_capture.cpp`) corre antes que el objeto
  del juego. Graba lo que dieron los mandos o lo reemplaza (reproducción, y en F3 la entrada remota del usuario 1),
  igual que hace el online del juego.

El SDK implementa `XamInputGetState` (xam `0x191`) en `sdk/src/kernel/xam/xam_input.cpp` y lo pasa a los drivers del
`input_system()`. `app/src/mk9_script_input.cpp` (mk9-nx) ya registra un driver propio para el mando por guion de las
pruebas.

## Línea de comandos del juego

- `appCmdLine()` = `sub_826CCB88`, devuelve el búfer `0x83415FA0`; `FEngineLoop::PreInit` lo llena con la línea que
  recibe el `main` (sin el nombre del ejecutable). `Parse(stream, "x=", out)` = `sub_826D25F8`.
- El SDK la arma en `sdk/src/kernel/xboxkrnl/xboxkrnl_module.cpp` (`ExLoadedCommandLine`): `"default.xex" <cl>`, con
  la opción `cl` del SDK (`--cl="..."` o `REX_CL`). Sirve para pasarle al juego opciones de desarrollo.
- Opciones encontradas: `bgnd=` (escenario; acepta `random`), `pchr1=` / `pchr2=` (personajes) — `sub_82386AE0` y
  `sub_82386D00`, guardan en la estructura de la partida `0x833C6C54` (`+28` -> objeto con el nombre del escenario en
  `+60`; jugadores en `+3304` y `+6516`). Las llama `sub_82386DF8` y `sub_82395B48`. Y las del online:
  `ONLINE_FENCEPOSTS=`, `MKONLINE_TRACE=`, `MWNET_TRACE=`, `MKONLINE_LOGFILE=`, `DESYNC_IP=`.

## El online propio del juego (mwNet + MKOnline)

Rutas de compilación en la imagen: `r:\MK9_Goty\integration\GameOfTheYear\Development\Src\MKOnline\...` y
`..\..\mwNet\Src\Core\...` (EndPoint, PacketQueue, EmulationDevice). Es el mismo netcode que estudiamos en la versión
de Vita (lockstep con retraso adaptativo, un paquete por cuadro, hash de estado de 4 bytes por cuadro).

| Qué | Dónde | Evidencia |
|---|---|---|
| Detector de desync por "fenceposts" | `sub_82A47688` (`OnlineDesync::Initialize`) | texto `821724E8`; reserva N entradas de 28 bytes; puntero `0x8342E810`, cantidad `0x8342E814` |
| Opción `ONLINE_FENCEPOSTS=` | `sub_823A5FE0`, `sub_824E4938` | texto `820108C4` |
| Volcado de desync `ONLINEDUMPDESYNCSESSION` / `ONLINEDUMPDESYNC` / `DESYNC_IP=` | `sub_823A8A80` | textos `82011AD8`, `82011AF0`, `82011ACC` |
| Mandos del online: `MKOnline_SetupGamepads` | `sub_82407350` | textos `82021070` / `82021050` ("[before]" / "[after]") |
| `MKOnlineGamepadData::m_RemoteGamepads [%s]` / `m_LocalGamepads [%s]` (volcado) | `sub_82408BB0` | textos `82021490`, `820214D0` |
| Emulador de red interno (latencia, pérdida) | `EmulationDevice` de mwNet; `MKOnline_EmulationDeviceSettingsTo` | textos `82171F20`..`821720F8`, `8201009C` |
| Sesiones de Xbox Live | `XSessionCreate`, `XSessionSearchEx`, `XSessionArbitrationRegister`, ... | textos `82043D18`, `8204A278`, `8204A3D8` |

## Memoria y RNG

| Qué | Dónde | Evidencia |
|---|---|---|
| `GMalloc` (allocator global de UE3) | `0x833D9074` | `OnlineDesync::Initialize` llama a su vtable `+8` (Malloc(size, align)) y `+16` (Free) |
| `rand()` del CRT | `sub_82B2CFB0` | LCG 214013 / 2531011 sobre `+20` del bloque por hilo (`sub_82B29580`); 50 llamadores, casi todos middleware |
| Otros LCG con las mismas constantes | `sub_82DE1DD8`, `sub_82E0E318` | constantes armadas en el código |

## La partida y los jugadores

| Qué | Dónde | Evidencia |
|---|---|---|
| Info de la partida (estática; el `GFG_GAME_INFO` de MK9Hook en PC es un puntero a la misma clase) | `0x833C6C54` | `pchr1=`/`bgnd=` escriben ahí; el chequeo de fenceposts la lee |
| Escenario | `[0x833C6C54+28]+60` (nombre) | `sub_82386AE0` (`bgnd=`), `sub_824022D0` |
| `GetInfo(gameinfo, jugador)` -> info del jugador | `sub_8237DB40` | `sub_824022D0` la llama con 0 y 1; en PC es `0x80E540` (MK9Hook) |
| Nombre del personaje | info `+2404` | igual que `GetCharacterName` de MK9Hook en PC: la clase tiene el mismo layout |
| ¿Modo Tag? (compañeros) | `0x833C6C54+13436` | `sub_824022D0` sólo agrega los compañeros si no es 0 |

Como el layout coincide con el del .exe de PC, los desplazamientos de MK9Hook son buenas pistas para el estado de la
pelea: súper barra info `+0xC14` (float), escala de daño info `+0x17EC`, objeto del luchador = `GetObj(info, 0)`
(PC `0x895B70`), vida objeto `+0x78C0` (float), velocidad objeto `+0xF4`, actor = objeto `+8`, posición actor
`+0xDC` (FVector). A confirmar en la 360.

**Fenceposts del online:** sólo 7 llamadas, todas en MKOnline. `sub_824022D0` (5) es el chequeo de que los dos lados
arman la misma pelea: CRC (`sub_82A96678`) del escenario y de los personajes, pasado a `Fencepost` (`sub_82A47600`).
`sub_824038D0` (2) es el otro usuario. El hash de estado por cuadro del lockstep es otra cosa (por ubicar).

## Pelea y scripts

- La lógica corre en el lenguaje de scripts propio de NetherRealm (archivos `.mko`: `gamelogic.mko`,
  `FightEngineScript.mko`, `FGFightEngineScript.mko`, `Moving.mko`, `Jumping.mko`, `BlockingDucking.mko`,
  `SharedReactions.mko`, `MKAI.mko`, `versus.mko`, `attract.mko`, ...). La charla de la GDC 2018 dice que es un
  script basado en fibras; su estado vive en la memoria del juego, así que una instantánea por páginas lo incluye.
- `GFGGameInfo` (clase del modo de pelea): `sub_8237D358` (texto `820038BC`).
- `GameSpeed`: `sub_828D6F18` (texto `82138AB8`); en el .exe de PC MK9Hook escribe la velocidad en un float global.

## Pendiente (F1)

1. Dentro de `FEngineLoop::Tick`: dónde avanza la pelea (un tick por cuadro) y qué hace el hilo de render.
2. El RNG de la pelea (el del script) y los cosméticos.
3. Los hilos: juego, render de UE3, FMOD, streaming, workers del sistema de jobs (`mk9_jobs.cpp`).
4. Los allocators (memoria física, Scaleform) y los rangos de memoria del juego para la instantánea.
5. Probar `pchr1=`/`pchr2=`/`bgnd=` con `--cl` y detectar el inicio y el fin de cada round.
