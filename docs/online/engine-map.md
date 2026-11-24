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

## Aleatorios

| Generador | Estado | Quién lo usa | Semilla original |
|---|---|---|---|
| El del juego: LCG `Seed * 196314165 + 907633515` (`sub_826D91C0` entero, `sub_826D91E8` flotante; reciben (archivo, línea) del llamador: rastreo de depuración de NRS) | `0x834161D8` | los scripts (nativas de `UtilsCore.cpp:2497`, IA, escalera) y el tick de `MK9Game.cpp` (un sorteo por cuadro, línea 628) | `sub_826D3088`, que pasa la semilla por la interfaz de captura (tag `0x00FF0004`) antes de guardarla: así el online de NRS daba la misma a las dos consolas. La llama `sub_82395B48` con el contador de ciclos |
| `appSRand` de UE3 (el mismo LCG, ~70 copias en línea) | `GSRandSeed` `0x833ED6FC` | motor (partículas, sonido) | pre-init `sub_82365A40` con el contador de ciclos (o 0 con `BENCHMARK`) |
| `GRandom` de Scaleform (multiply-with-carry de Marsaglia, 8 palabras, acarreo 362436) | `0x833AFA84` (+32 acarreo, +36 índice) | ActionScript `random` | init estático `sub_83367A08` con el tick count; `sub_82F89198` = SeedRandom |
| `rand()` del CRT | por hilo | sólo middleware (audio y otros), no el juego | `srand` sólo desde el init del audio |

`app/src/online/seed.cpp` los siembra a todos con un valor (la semilla de la sesión, o `mk9_seed`) y reemplaza la
semilla de cada modo que pasa por el tag 4 por la siguiente de una secuencia derivada.

## El canal de sincronía de NRS (la interfaz de captura)

`sub_82A1C550(tag, data, size, user)` = vtable+4 del objeto en `0x8342D01C` (Práctica, el online del juego);
`sub_82A1C5C0(tag, valor)` = vtable+8 (valores a comparar entre consolas); `sub_82A1C6B0` = vtable+24 (inicio de
cuadro); `sub_82A1C610(x)` = vtable+16. Tags: 1-3 mandos; 4 semilla de modo; 6 (`sub_82728510`); 7 (`sub_829FB5C8`);
8, 9, 10 los chequeos por cuadro del tick de `MK9Game.cpp` (`sub_82382E40`): 9 y 8 = info de la partida +108/+112
(cuadros desde el arranque y desde el inicio del modo; el 8 vuelve a 0 en cada cambio de modo), 10 = un sorteo del
RNG del juego; 11 y 13 al instalar un grabador (`sub_823D98D8`, `sub_823DB1C0`: datos de cada jugador +3092). Los
scripts también la usan con la nativa `8247cd50` (`sub_8247CD50`) y chequean con `sub_8247CD70`.

## Scripts MKO

- La VM: contexto en `[0x8342CDF0]` (pc en +60, registro de resultado de una nativa en +28). `sub_82A12118` = llamar
  a una nativa (su dirección está en el bytecode, después de la instrucción). `sub_82A164C8` corre las 21 ranuras de
  scripts cada cuadro desde el tick de `MK9Game.cpp`; `sub_82A15FC8` ejecuta una fibra.
- El C++ arranca funciones de script por nombre: `sub_82A259C0(script, nombre)` = `sub_82A10380` (buscar en la tabla
  del script: +32 cantidad, +140 entradas, cada una con su nombre en +0) + `sub_82A23D50(script, número, ...)`.
- Nativas útiles: `8246ec38` = `IsAsyncLoading` (`sub_8270BF88`: `GObjAsyncPackages.Num > 0`); `824747c0` =
  `IsMoviePlaying` (`sub_829CE600`: `GFullScreenMovie` en `0x834246A8`, +20 y la bandera de `0x834246B4`);
  `824730b8` = la máquina de estados de carga de la pelea (`sub_82464410`, estado en `0x833CF9E8`); `824a5268` = "la
  escena está lista" (`sub_82A30588`); `82495660` = carga `Toasty.mko` y corre `toasty_init` al empezar cada pelea;
  `824705F8` = random del script.

## Cargas

`UObject::LoadPackageAsync` = `sub_82730F70` (agrega a `GObjAsyncPackages`, TArray en `0x834162A4`, elementos de 96
bytes); `UObject::ProcessAsyncLoading(bUseTimeLimit, TimeLimit)` = `sub_82728848`; `UObject::FlushAsyncLoading` =
`sub_8272EC30`; `IsAsyncLoading` = `sub_8270BF88`. Los cargadores propios de MK9 (`sub_82A20DD8`, `sub_82A20F00`)
también usan `LoadPackageAsync`. `GIOManager` en `0x833DD540`.

Flujo de la carga de una pelea (un cuadro = 1/60 s; Ladder, una corrida): el modo empieza (tag 8 vuelve a 0) ->
espera `IsAsyncLoading` -> pantalla VS -> espera la máquina de estados `824730b8` (~50 cuadros) -> `IsAsyncLoading`
-> espera `824a5268` (~20 cuadros, con la nativa de captura cada cuadro) -> `IsAsyncLoading` -> `82495660`
(`toasty_init`, `entry`, `level1_activate`): empieza la pelea (~150 cuadros después del inicio del modo).

## El tiempo y lo que lo lee (2026-11-11, `virtual_time.h`)

| Qué | Dónde | Evidencia |
|---|---|---|
| El reloj del invitado (`mftb`) | `REX_QUERY_TIMEBASE()` = `Clock::QueryGuestTickCount()` | `mk9_pch.h`; 50.000.000 ticks por segundo |
| `appCycles` del motor (hilo del juego; muchas veces en línea) | `sub_82366BF8` | `GSecondsPerCycle` en `0x833DD0A0` |
| Espera del tick a la cerca de comandos del render | `sub_82741630` | lee el reloj sólo si espera |
| Espera del tick al render con carga asíncrona en tajadas de 0,01 s | `sub_82360480` | llama a `sub_827413C0`, `appSleep(0)` `sub_8255D030` y `ProcessAsyncLoading(1, 0.01)`; suma la espera en `0x833DD558` |
| `IncrementalPurgeGarbage(bUseTimeLimit, TimeLimit)` | `sub_826D3CD8` | lee la hora de inicio y le pasa el presupuesto a la primera pasada |
| Primera pasada de la purga: objetos que esperan que el render los suelte | `sub_82568540` | objeto en `[0x833914C8]`, pregunta vtable `+44` al mismo objeto hasta que dice que sí, lee el reloj cada 10 preguntas |
| Nativa "cuadros que le faltan a un sonido" | `824a5610` -> `sub_82A33C00` | (vtable `+336` − vtable `+300`) × `sub_826CC678` (60) |
| Busca un componente de audio por id | `sub_82A31410` | entre los del dispositivo de audio (`sub_82602A98`, lista en `+640` vía `sub_825AD718`), el que tiene el id en `+168` |
| Componente de audio de MK9 | vtable `0x820D6A30` | evento de FMOD en `+264`; posición guardada en `+156` |
| Largo de su evento (s) | vtable `+336` = `sub_82604DD8` | `lengthms` de `getInfo` × 0,001 (−1: sin largo) |
| Posición de su evento (s) | vtable `+300` = `sub_82589898` | `positionms` de `getInfo` × 0,001: el reloj del mezclador de FMOD |
| `FMOD::Event::getInfo(index, name, FMOD_EVENT_INFO*)` | `sub_82E46D40` | la info: `+4` `positionms`, `+8` `lengthms`, `+40`/`+44` instancias |
| `FMOD::Event::start` | `sub_82E46DE8` | hook en `sound_replay.cpp` |

## El luchador en memoria (2026-11-11, `mk9_fighter_dump`)

Vida: objeto `+0x78C0` (float, 1 llena). Posición del actor (`[objeto+8]`): `+0xDC` FVector, `+0xE0` el eje de la
pelea, `+0xE4` la altura. Lo que es igual en las dos máquinas y cambia en una pelea está en
`app/src/online/fight_hash_ranges.inc` (lo genera `tools/online/dev/fighter_diff.py`). Palabras con ids `0x1000xxxx`
(contador global, no punteros) aparecen en el objeto y en la info y pueden diferir entre máquinas.

## Pendiente (F1)

1. Dentro de `FEngineLoop::Tick`: dónde avanza la pelea (un tick por cuadro) y qué hace el hilo de render.
2. El RNG de la pelea (el del script) y los cosméticos.
3. Los hilos: juego, render de UE3, FMOD, streaming, workers del sistema de jobs (`mk9_jobs.cpp`).
4. Los allocators (memoria física, Scaleform) y los rangos de memoria del juego para la instantánea.
5. Probar `pchr1=`/`pchr2=`/`bgnd=` con `--cl` y detectar el inicio y el fin de cada round.

## Frame data (2026-11-17, `mk9_frame_data.cpp`)

Encontrado con `mk9_fighter_dump` cuadro por cuadro en Práctica (Scorpion contra Sub-Zero) y comprobado contra las
tablas de la comunidad de MK9 (1, 2, 3, 4, d1, d3, d4 y f4 de Scorpion: arranque, total y ventaja al golpear y al
bloquear, iguales). Todo en el objeto del luchador:

| Qué | Dónde | Valores |
|---|---|---|
| Lo que hace | `+0x1D4` | el tipo en los bits 8-15: `0x52` ataque de pie, `0x53` agachado, `0x46`/`0x47` golpeado, `0x0A` cubriéndose, `0x20` caminando o saltando (`0x2000` adelante, `0x2001` atrás, `0x200E`/`0x2009`/`0x200F` salto), `0x01` agachado; 0 parado. Con `0x40` en el tipo no puede actuar: el ataque y el aturdimiento terminan cuando sale de ahí |
| El ataque | `+0x188` | id del golpe (`0x1000xxxx`, -1 sin golpe); cada golpe tiene uno, un golpe en dos partes (el 4) cambia antes de tocar |
| Fase del ataque | `+0x2F74` | 1 arranque, 2 activo, 3 recuperación; al tocar pasa a 3 en el acto. El arranque = primer cuadro con 2 o 3 |
| Animación | `+0x2D4` | float: cuadro de la animación actual, -1 en el primero de una nueva. Cubriéndose, vuelve a -1 al recibir el golpe y otra vez al terminar el aturdimiento del bloqueo (el tipo sigue en `0x0A`) |
| Daño hecho (acumulado) | `+0x3014` | float (0,03 = 3 %) |

En la pausa del juego nada se mueve (las dos animaciones quietas): esos cuadros no cuentan. El contador de cuadros de la
partida (+108) sigue corriendo en la pausa.

## Grabar al rival en Práctica (2026-11-18, `mk9_practice_record.cpp`)

Lo pidió Irrit8 con una donación. La grabación propia del juego (pausa > GRABACIÓN/REPRODUCCIÓN, 8 lugares) graba al
luchador del jugador; ésta graba al muñeco y se la hace repetir, como el modo de entrenamiento de los juegos de pelea
de después.

- **El muñeco con un mando**: con pausa > OPCIONES DE IA > TIPO DE ENEMIGO: HUMANA (`[Pause] EnemyType`/`Human` en
  `Coalesced.*`), el muñeco sigue al usuario 1 de XInput. El juego lee los cuatro mandos en cada cuadro por la
  interfaz de captura (`sub_82A1C550(tag, data, size, user)`): `0x00FF0002` de 16 bytes es el `X_INPUT_STATE` (número
  de paquete y el gamepad de 12, big-endian), `0x00FF0003` de 4 el resultado de `XInputGetState` (un temporal en la
  pila: `data - r1 < 0x400`) y `0x00FF0002` de 20 las capacidades. Mientras graba o reproduce, el usuario 1 aparece
  conectado (resultado 0, las capacidades de un mando común, como el invitado de una sesión: `netplay::OverrideBlock`)
  y recibe las entradas; el número de paquete cambia cuando cambia el gamepad, como cuenta XInput.
- **Grabar**: el gamepad del usuario 0 (mando o teclado) pasa al usuario 1 y el del 0 queda en cero (el luchador del
  jugador quieto); sin START ni BACK (el muñeco no pausa). START del usuario 0 termina la grabación o la reproducción y
  sigue al juego (la pausa es del jugador). Hasta 3600 cuadros (60 s).
- **El lado**: el actor del luchador (objeto `+8`) tiene su lugar en `+0xDC`, `+0xE0`, `+0xE4` (x, y, z, floats;
  `fight_state::Position`). Al empezar la pelea, (0, -125, 0) y (0, 125, 0): el costado de la pelea es la y. El
  código toma el eje y su signo en el primer cuadro de la pelea (el jugador 1 empieza a la izquierda) y guarda con cada
  cuadro grabado de qué lado estaba el muñeco; al reproducir del otro lado, cambia izquierda y derecha (la cruceta y la
  x del stick izquierdo): adelante sigue siendo adelante. Uno encima del otro (menos de 1 de distancia), el lado queda.
- **Teclas**: F5 graba, F6 reproduce, F7 en bucle; en el mando BACK + LB, RB o X (`overlay_input`, que en Práctica
  le saca al juego esos cuadros: LB y RB son el agarre y el relevo). Nunca en una sesión online (`SessionGame`).
- **Prueba**: `mk9_practice_record_test=17:record,23:record,28:play` (segundos desde el primer cuadro de la pelea) con
  el guion del mando que pone HUMANA (pausa, OPCIONES DE IA, RB RB, A) y mueve al muñeco. Visto en video: mientras
  graba, Sub-Zero camina y pega y Scorpion queda quieto; al reproducir repite lo mismo; con Scorpion saltando por
  encima antes de reproducir, Sub-Zero camina hacia la derecha (grabado hacia la izquierda) y lo alcanza.

## La selección en 3D (2026-11-24, `mk9_select_preview.cpp`)

F2 > Controles, "Personajes en 3D en la selección" (`mk9_select_preview`, apagada de fábrica, se prende y se apaga en
vivo): en la selección de personaje (estado 7) cada lado muestra a su luchador en 3D en vez de su ilustración, con el
traje que se está eligiendo, en guardia, y al elegirlo se burla de su rival. Sólo offline.

- **La escena**: la selección es la película de Scaleform `ui_ps_vs` sobre un mundo vacío (`empty_map`) con la cámara
  en el origen mirando +X. Las ilustraciones son `_root.ps.characters_team<N>.bodies_1`; el fondo, opaco, es el primer
  clip sin nombre de `_root.ps` (`instance241`: se encuentra probando nombres). Las dos se ocultan con
  `GFxMovieView::SetVariable` (vtable 16: ruta, `GFxValue{0, 2 = bool, valor}`, 0; da 1 si el clip existe). La vista sale
  del envoltorio del clip: `sub_82A375A0` da la raíz, su +88 la vista.
- **Qué luchador y qué traje**: `SetImage` (`sub_82A3B6C0`, la ruta del clip es el FString +8 del envoltorio: TCHAR de
  un byte) recibe `...BD_<luchador>[_f].jpeg` (`dlc`, `random`, `locked*` no son luchadores; los DLC traen la suya en
  `<Luchador>_ui`). El menú de trajes (START) localiza el paquete del traje en la sección `PlayerSelect`
  (`sub_82A235B0`, "CHAR_Scorpion_B"; el de fábrica es `char_<luchador>`) y escribe el texto en
  `_root.ps.AltCostumeSide<N>` (`sub_82A38648`).
- **El luchador**: el paquete con `UObject::LoadPackageAsync` (`sub_82730F70(FString*, 0, 0, 0)`), entero cuando
  `GObjAsyncPackages.Num` (`0x834162A8`) es 0. Nunca uno que el sistema de archivos no tenga: el juego se cierra
  (0xC0000409). La malla como la busca el armado del luchador (`sub_823CC2E8`): `Meshes.<luchador>` del traje, si no
  `SourceAsset`/`MeshName` de su sección del ini (GConfig `[0x833D9064]` vtable 7); los retro se visten con las MIC
  del traje (`<traje>.Materials.<nombre de la del aspecto>`, `SetMaterial`: vtable 124 del componente). El objeto MK
  1021 de los guiones (nativa `824826F8`): `sub_829DB018(464)` y `sub_829F69E0(this, malla, 0x7FFFF, 0, 0, 0)`; su
  actor en +8, su componente en +268; vtable 1 lo borra (1: libera), 29 toca una animación (anim, f1, f2 0.9 como el
  visor de biografías, f3, flags 2). Sin `bUseMKDynamicShader` (+0x120, bit 0x00100000) las luces de MK lo dejan
  negro; también `LightEnvironment` (+0x114) 0 y `MKOpaqueDetailCategory` (+0x130) 4. Tres luces propias
  `PointLightMovableSpawnable` (`UWorld::SpawnActor` `sub_828FEA18`; `PointLightMovable` es bNoDelete) con
  `AffectCharacters` (LightComponent +0x10C, 0x40000000), brillo +0x100, color +0x104, radio +0x194. El lugar en el
  componente (Translation +0x200, Rotation +0x20C, Scale3D +0x21C); el jugador 2 espejado (Scale3D Y -1).
- **El juego borra sus objetos MK** al terminar la selección (antes de cambiar de estado): el hook del destructor
  (`sub_829F6D98`) suelta los nuestros, que no se tocan más.
- **Memoria**: lo que se muestra queda en el root set (flags +12, 0x4000) y una recolección se pide como la Kripta (el
  +64 del objeto de estado `[0x833848D4]`, que `sub_82382E40` atiende en el mismo cuadro con
  `UObject::CollectGarbage` `sub_826DD740(RF_Native, +68)`) cuando nada carga; después las cargas esperan 20 cuadros.
  Sin eso, un paquete recién cargado perdía sus animaciones antes de mostrarse; sin recolección, recorrer la selección
  llenaba la memoria.
- **Las recolecciones en la selección** (2026-11-24, el bug de los cuadros naranjas sobre el HUD y los efectos en la
  pelea, capturas de jugadores de la 1.4.3):
  - El juego sólo recolecta en la selección cuando el cursor llega a los luchadores DLC: su administrador de DLC
    (`sub_8260C268`) llama a `CollectGarbage(RF_Native, TRUE)` **sin** `UObject::FlushAsyncLoading` (`sub_8272EC30`,
    que `sub_82700150` sí llama antes); en la consola no hay nada cargando ahí. La vista previa estaba cargando ese
    mismo luchador: la recolección se llevaba el paquete a medio hacer ("loaded but no mesh found" con Skarlet, Kenshi,
    Rain, Freddy) y el cargador seguía escribiendo en lo liberado. Ahora el hook de `sub_826DD740` termina las cargas
    pendientes (`FlushAsyncLoading`) antes de toda recolección en la selección y guarda en el root set, mientras dura,
    lo que la vista previa carga o muestra.
  - La recolección de la vista previa se llevaba también lo del juego (los menús `ui_s_mainmenu`, `ui_s_options`,
    `ui_s_dlcBillboard`) y los efectos que comparten todos los luchadores (`FX_Blood`, `FX_Impact`, `FX_Global`,
    `FX_Character`, `PFX_Xray`). Ahora, mientras dura, todo lo demás queda en el root set: sólo se van los paquetes con
    el nombre de un luchador que ya no se muestra (`CHAR_Scorpion-<hash>`, `Scorpion_XRAY`, `FX_Scorpion`,
    `SND_SFX_Scorpion`...) y las cinemáticas (`NIS_MK`, `ANIM_MK`, `SND_MOV_MK`).
  - Banderas de UE3 de esta versión (las 64 bits en +8): `RF_RootSet` 0x4000 y `RF_BeginDestroyed` 0x8000,
    `RF_FinishDestroyed` 0x10000 en la mitad baja; `RF_Unreachable` 0x2, `RF_NeedLoad` 0x200, `RF_AsyncLoading` 0x400,
    `RF_PendingKill` 0x20000000, `RF_Native` 0x04000000 en la alta. Las búsquedas de la vista previa saltean los que se
    están destruyendo o cargando.
  - Pruebas: `mk9_select_preview_test_gc_report` (qué se llevó cada recolección, por paquete y por clase); el log dice
    de quién es cada recolección en la selección.
- **La elección**: el trace de los guiones "Player %d: selected" (`sub_82479280`) sale también con START, que abre el
  menú de trajes sin elegir. Elegir es `this._parent.SelectionHighlight<N>.gotoAndPlay` del sprintf de los guiones
  (`sub_8246E430`: sus argumentos en el +60 de la VM `[0x8342CDF0]`, el primero el buffer del texto) con el trace en el
  mismo cuadro (A sobre un luchador: el jugador del trace; N no es el jugador: el 2 marca el 4) o seguido de
  `ps.AltCostumeSide<lado>.gotoAndPlay` (A en el menú de trajes).
- **Las actuaciones** (`mk9_select_performances.inc`, hecha por `tools/select3d/perf_tables.py` con las cinemáticas
  del juego): las transiciones del modo Historia (`NIS_MK<nnn>_PRE` y `_PST`: antes y después de cada pelea uno le
  habla al otro; quién es lo dice su animación de cara, `..._<Quién>_FO`; su cuerpo y su cara salen de las claves de
  `CinemaTrackBlendAnim` y su frase, `snd_vo_storytrans.vo_mk9_<nnn>_<pre|pst>`, de las de `InterpTrackSound`) y las
  presentaciones de antes de la pelea (`<Luchador>_CharIntro`: cuerpo, cara, cada evento de sonido y las voces del
  personaje de `CinemaTrackCharacterSound`, como `@vo_spatk`). Las propiedades de esta versión: la etiqueta de un
  BoolProperty lleva un UBOOL de 32 bits y la de un ByteProperty no lleva enum. Al mostrarse, una al azar entre todas
  las del luchador (y su `EndOfRound_Taunt` con una frase de su voz) carga su paquete; al elegirlo, el cuerpo, los
  sonidos y las frases a su tiempo desde el comienzo del cuerpo, y la guardia otra vez al final.
- **Más frases: las de las escenas de Historia** (`SND_MOV_MK<nnn>` y sus doblajes `_spa`, `_fra`, `_ger`, `_ita`): un
  evento por frase con el nombre de quién la dice (`vo_mk006_sony02`: Sonya; `vo_mk9_020_scor02`: Scorpion; los
  códigos como en `SND_VO_CharIntro`: `cyrh` Cyrax, `sekh`/`sekt` Sektor, `sub` Sub-Zero; `shay` y `goro` no están en
  la selección). 96 frases de 20 luchadores, dichas con el `EndOfRound_Taunt` del luchador. Los eventos del doblaje
  están dentro del paquete `SND_MOV_MK<nnn>` del archivo `_spa`: `FindEvent` busca el paquete en cualquier nivel de
  los outers. El sistema de sonido los toca con el nombre del archivo (`SND_MOV_MK006_spa`).
- **El idioma de lo que dicen**: el del juego (`user_language` con que arrancó: 5, 4, 3, 6 doblados; los demás inglés).
  Las frases de las presentaciones (`SND_VO_CharIntro`) y las burlas habladas del luchador sólo existen en inglés: con
  otro idioma, la presentación va sin su frase y la burla con su risa (`vo_laugh`). Un jugador lo pidió: "las voces se
  ponen en inglés cuando deberían estar según lo tengas configurado".
- **Otras poses**: el paquete del luchador trae `Stance_Alt01`-`05` (no todos) e `Idle1`; mientras se muestra, una cada
  7 a 13 s entre sus guardias.
- **Las frases**: `SND_VO_StoryTrans` viene en inglés y en `_spa`, `_fra`, `_ger`, `_ita`: la del idioma con que
  arrancó el juego (`user_language` 5, 4, 3, 6; los demás en inglés); `SND_VO_CharIntro` sólo en inglés. Se cargan al
  abrir la selección y quedan en el root set mientras dura (FMOD las lee del archivo: "FMOD streams ... from the
  host"). Suenan por el sistema de sonido de MK (`sub_82A30268`, vtable 9) con la referencia {nombre del paquete, FName
  del evento, 0, -1, nombre}, como el guion toca al locutor. Comprobado grabando la salida (`audio_test_fake_device` y
  `audio_dump_output_s`): con la frase, 5 a 7 veces más energía en los segundos que dura.
- **Mirando al rival**: las transiciones miran a la cámara de su cinemática (el rival al costado) y las presentaciones
  están hechas para la pelea (de perfil, mirando al otro lado): mientras actúa, el luchador gira 45° o 135° hacia su
  rival (5° por cuadro) con la Rotation del componente y su actualización (`sub_828CB1E0`, como
  `UPrimitiveComponent::execSetRotation`, `0x82739398`), y vuelve al terminar.
- **Las que se van de su lugar**: algunas terminan lejos de donde empezó la guardia (la animación mueve la cadera: se
  irían y después volverían de un salto a la guardia). `mk9_select_preview_test_survey` toca una por una en el
  jugador 1 y mide la cadera sobre el piso desde su lugar en la guardia
  (`USkeletalMeshComponent::GetBoneLocation`, `sub_82924108(out, componente, FName, 0)`; los nombres de los huesos
  son el `TArray<FName>` +0x90 de la malla: Reference, Hips, Spine, ...; la raíz no se mueve: todo está en la
  cadera; `mk9_select_preview_test_survey_from` empieza en otra). Las presentaciones y algunas escenas entran desde
  fuera de cuadro (Scorpion a 187 unidades, Mileena a 381) y terminan en su lugar: ésas quedan. Muchas de después
  de la pelea terminan a 40-55 unidades, casi todo hacia la cámara (vuelven a la guardia sin salto visible).
  `perf_tables.py --survey <log> --max-end 55 --max-length 16` deja fuera las que terminan más lejos (10: la
  presentación de Baraka termina a 406) o duran más de 16 s (2: una de Kabal dura 41 s). Quedan 133: 103 frases de
  Historia (24 luchadores) y 30 presentaciones; los 31 luchadores tienen al menos una, además de su burla.
- **La cara no se mueve al hablar**: las pistas de cara usan la subpista "Face Only" con los huesos `F_*`, y las
  mallas sólo tienen las subesqueletos FullSkeleton, ArmCloth, WaistCloth y DamageCloth (datos MK del componente +656:
  +168 entradas de 40 bytes {FName, máscara de 256 huesos}, +172 cuántas, +176 lugar: 4 de 4). El slot 34 del objeto
  MK toca en una por nombre (el 29 lo llama con "FullSkeleton"); una de cara obligaría a agrandar ese arreglo.
- **Pruebas**: `mk9_select_preview_perform` (el paquete de una actuación, `taunt` o `none`),
  `mk9_select_preview_test_pick=2:600` (una elección en ese cuadro de la selección, sin la del juego),
  `mk9_select_preview_test_toggle=600,900` (la opción dada vuelta en esos cuadros), `mk9_select_preview_turn`,
  `mk9_select_preview_place`, `mk9_select_preview_lights`, `mk9_select_preview_test_survey`. El registro de nativas con
  sus argumentos (`mk9_native_log_args`) mostró la elección y el menú de trajes.
