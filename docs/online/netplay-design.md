# Diseño del online (F3-F5)

Borrador de 2026-11-07, con lo que encontramos en F1 (engine-map.md). Se ajusta cuando tengamos el juego corriendo y
las pruebas de determinismo (F2).

## Dos caminos

### A. Versus local + netcode nuestro (recomendado para empezar)

Es lo que hace Tekken 6 Native Online ("offline Versus + GGPO, like SF4 Ember Netplay"): el juego corre una pelea
local de dos jugadores y el jugador 2 es la entrada que llega por la red.

- **Entrada:** la lectura de mandos de cada cuadro (`sub_829D5230`, una vez por cuadro, en el hilo del juego, antes de
  `FEngineLoop::Tick`) es el límite de cuadro. La entrada del jugador remoto entra por la interfaz de captura
  (`sub_82A1C550`, bloques `0x00FF0002`/`0x00FF0003` del usuario 1), que ya enganchamos para F2. Es el mismo camino
  que usa el online del juego: `sub_824088A0` (MKOnline) instala su objeto en `0x8342D01C` y reemplaza ahí los
  bloques del jugador remoto.
- **Setup de la pelea:** las opciones `pchr1=`, `pchr2=` y `bgnd=` de la línea de comandos (`--cl`) fijan personajes
  y escenario. Si sirven para entrar a la pelea directo, la elección se hace en nuestra sala (como en el MK9 de Vita)
  y los dos juegos arrancan la misma pelea. Si no, hay que sincronizar la pantalla de selección en lockstep, con las
  cargas esperadas en los dos lados (Tekken 6: `WaitLoaderIdle`).
- **Lockstep (F3):** cada cuadro espera la entrada remota del cuadro `N` antes de soltar la lectura de mandos;
  retraso automático según el ping (RTT/2 + 1 cuadro, lo que en Vita quedó en "Moderado").
- **Rollback (F5):** GGPO (MIT, el fork de Tekken 6 encaja con ReXGlue) o GekkoNet (BSD-2). La instantánea va en el
  mismo límite de cuadro (F4).
- **A favor:** controlamos todo el cuadro; no depende del online del juego (Xbox Live, sesiones, arbitraje).
- **En contra:** hay que rehacer lo que el online del juego ya trae (selección sincronizada, revancha, abandono).

### B. El online propio del juego (mwNet + MKOnline) sobre nuestro transporte

Es lo que hizo el port de TTT2 con el NACE del juego y lo que hicimos en Vita con el Ad Hoc: el juego cree que está en
Xbox Live y sus paquetes viajan por nuestra red.

- Hay que emular las sesiones de Xbox Live (`XSessionCreate`, `XSessionSearchEx`, `XSessionJoinRemote`,
  `XSessionArbitrationRegister`, ...), el QoS (`XNetQos*`) y los sockets VDP del juego.
- El juego trae su lockstep con retraso adaptativo y su detector de desync (fenceposts, hash de estado por cuadro),
  sus menús online, revancha, "King of the Hill" y espectadores.
- Para el rollback habría que reemplazar la sincronización de entradas de mwNet por la nuestra (como TTT2 con NACE).
- **A favor:** todos los modos online del juego, con su interfaz.
- **En contra:** mucha superficie de Xbox Live para emular; el rollback igual hay que meterlo adentro.

Plan: A primero (F3 jugable con lockstep, después rollback), B como mejora si vale la pena.

## Transporte

- WebRTC nativo con [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (MPL-2.0): canal de datos
  no ordenado y sin reenvíos (un paquete por cuadro con las últimas entradas, como el juego), señalización por el
  lobby del VPS (`wss://107-172-143-35.sslip.io/lobby?game=mk9pc`) y TURN del VPS (`/ice`) cuando no hay conexión
  directa. Es la misma infraestructura que usan BT3, Tekken, Def Jam y Tazita.
- Alternativa: UDP propio con hole punching vía el VPS y relay.

## Determinismo (F2) — logrado en lockstep (2026-11-07)

Dos instancias en lockstep desde el arranque (`tools/online/pair_test.ps1`) llegan al mismo cuadro de inicio de pelea
y pelean 20 s (Scorpion contra la IA, que sortea todo el tiempo) sin una sola diferencia en los chequeos del juego,
cinco corridas de cinco. Lo que hizo falta, en orden de aparición:

1. **Semillas** (`seed.cpp`): las del juego, UE3 y Scaleform salen del reloj; la sesión las fija con la semilla del
   host (HELLO) y reemplaza la semilla de cada modo (tag 4 de la interfaz de captura).
2. **Cargas** (`loading.cpp`): un paquete termina de cargar en un cuadro que depende del disco y la CPU de cada
   máquina, y los scripts lo esperan. En sesión, al empezar cada cuadro se terminan las cargas pendientes
   (`FlushAsyncLoading`): lo que pide un cuadro está listo en el siguiente en las dos. Cuesta poco (un arranque hasta
   la pelea: 13 vaciados, 37 paquetes, 663 ms en total, el más largo 142 ms).
3. **Películas** (Bink): duran tiempo real. `IsMoviePlaying` viaja como bandera "ocupado" con las entradas: el juego
   la ve terminar cuando terminó en las dos máquinas (`Busy`).
4. **"La escena está lista"** (nativa `824a5268`): bandera "listo" por consenso: el juego la ve lista sólo cuando las
   dos máquinas dijeron listo en el cuadro anterior a la salida de las entradas (`Ready`, ~3 cuadros de espera).
5. **Barrera al empezar la pelea** (`Barrier`, nativa `82495660`): nueva época de la sesión, resiembra y cuenta de
   cuadros desde 0; si una máquina llega antes espera a la otra, que sigue con mandos neutros. Es el lugar del
   `GAME_START_GO_REQUEST` del online de NRS; con lo anterior las dos llegan en el mismo cuadro.

El chequeo de desync es el del propio juego: los valores de los tags 8, 9 y 10 de cada cuadro (y opcionalmente
rangos de memoria, `mk9_net_sync_ranges`) van hasheados con las entradas; un cuadro distinto se avisa (`DESYNC`).

Herramientas para encontrar la próxima fuente de diferencia: `mk9_rand_log` (cada sorteo con su pila;
`tools/online/rand_report.py A --other B` dice el primero distinto), `mk9_check_log`, `mk9_script_log` (funciones de
script que arranca el C++), `mk9_native_log` + `mk9_native_log_frames` (nativas de una ventana de cuadros con su
resultado: la primera que da distinto en las dos máquinas es la culpable), `tools/online/func_strings.py`.

Pendiente: la sesión de verdad no empieza en el arranque sino en un menú (las dos máquinas con historias distintas):
barrera al entrar a la selección del versus; probar con dos personas apretando; Tag; peleas largas.
