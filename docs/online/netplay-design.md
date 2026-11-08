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
4. **Esperas de los scripts que terminan con el reloj de cada máquina**: bandera "listo" por consenso (`Ready`): el
   juego la ve terminada sólo cuando las dos máquinas dijeron terminada en el cuadro anterior a la salida de las
   entradas (~3 cuadros de espera). Hoy: `824a56d0` (la compuerta de la selección del Versus al modo pelea),
   `8247bf38` (streaming del motor ocupado) y `824a5268` (la escena lista, la última antes de la pelea).
   `AgreeOnNativeResult` en `mk9_capture.cpp` hace una nueva con dos líneas.
5. **Barrera al empezar la pelea** (`Barrier`, nativa `82495660`): nueva época de la sesión, resiembra y cuenta de
   cuadros desde 0; si una máquina llega antes espera a la otra, que sigue con mandos neutros. Es el lugar del
   `GAME_START_GO_REQUEST` del online de NRS; con lo anterior las dos llegan en el mismo cuadro.

El chequeo de desync es el del propio juego: los valores de los tags 8, 9 y 10 de cada cuadro (y opcionalmente
rangos de memoria, `mk9_net_sync_ranges`) van hasheados con las entradas; un cuadro distinto se avisa (`DESYNC`).

Herramientas para encontrar la próxima fuente de diferencia: `mk9_rand_log` (cada sorteo con su pila;
`tools/online/rand_report.py A --other B` dice el primero distinto), `mk9_check_log`, `mk9_script_log` (funciones de
script que arranca el C++), `mk9_native_log` + `mk9_native_log_frames` (nativas de una ventana de cuadros con su
resultado: la primera que da distinto en las dos máquinas es la culpable), `tools/online/func_strings.py`.

**Versus de dos jugadores (2026-11-07):** el menú principal pasa a "2 PLAYERS" con RB; el jugador 2 se une con START
estando ya en ese menú (antes, VERSUS MODE está en negro) y el cursor salta solo a VERSUS MODE; cada uno elige con A.
`tools/online/fight_script.py` arma los guiones (menú + pelea con golpes, bloqueos y saltos) y
`tools/online/hunt.ps1` repite la prueba hasta que algo se separe y guarda los registros. 8 de 8 corridas de un Versus
con los dos pegándose (~35 s de pelea cada una) sin una diferencia.

**Sesión desde el arranque:** como el determinismo vale desde el cuadro 1, el modelo más simple y robusto es que cada
partida online arranque el juego ya conectado (como los ports de Tekken): el lobby arregla la partida, el juego se
relanza en sesión con un perfil neutro (los guardados de cada uno tienen desbloqueos distintos) y los dos menús
avanzan juntos. Sumar a una sesión a mitad de partida exigiría dos estados iguales, que no hay.

Pendiente: perfil neutro; saltear la intro; ir solo hasta la selección del Versus; Tag; peleas largas y revancha.

## Rollback (F4) — el núcleo anda (2026-11-07)

`app/src/online/snapshot.cpp`. La memoria de la simulación es el heap virtual por debajo de `0x70000000` y los datos
de la imagen (`0x80000000-0x9FFFFFFF`). Quedan afuera las pilas, TLS y objetos del kernel (`0x70000000-0x7FFFFFFF`),
el heap bajo (kernel y XAM) y los heaps físicos (texturas, búferes de GPU y audio, cosa de Renut).

- **Restauración completa** (`mk9_rollback_test=F,K`): con el render de UE3 vaciado (`FlushRenderingCommands`,
  `sub_82747FE0`), restaurar 88 MB y volver a correr 30 cuadros de pelea contra la IA da los mismos chequeos y los
  mismos 89 sorteos. Es la prueba de que el rollback por memoria es posible.
- **Línea de tiempo** (`Timeline`): las *store cards* (cada store del juego pone en 0 el byte `ea >> 12` del primer MB)
  dicen qué páginas se escribieron desde el último cuadro. Renut sólo lee las de los heaps físicos, así que éstas las
  consume la línea de tiempo. Cada página escrita deja su contenido anterior (de una copia sombra) en el registro de
  deshacer del cuadro. Retroceder restaura cada página una vez, desde el registro más viejo que la tiene.
- **Lo que las cards no ven** se cubre así:
  - los stores que marcan la card *antes* del dato (vectoriales, `dcbz`, el CRT del SDK), hechos por otro hilo justo
    mientras se consumen las cards: se vuelven a comparar las páginas del cuadro anterior;
  - lo que escribe el host sin card (búferes de audio, lecturas de archivo): se aprende comparando todo durante los
    primeros 2 s y después se compara cada cuadro.

  Los objetos del kernel que viven en la memoria del juego nunca se revierten: son "agujeros" sacados de la tabla de
  objetos del SDK (hoy no hay ninguno en el rango).
- **Renut:** mientras el cuadro vacía el render de UE3, la compuerta de Renut se mantiene abierta (`HoldGateOpen`). Si
  no, el render de UE3 esperaba una cerca de GPU detrás de la compuerta, la compuerta esperaba el Swap del juego, y el
  juego esperaba el vaciado: un cuelgue de 3 s hasta el watchdog.
- **SyncTest** (`mk9_synctest=F,P,K`): cada P cuadros retrocede K y compara los chequeos del juego de cada cuadro que
  vuelve a correr. 125 rollbacks de 6 cuadros, 744 cuadros comparados, 0 diferencias, a 60 fps. Guardar un cuadro
  (~1700 páginas) cuesta ~4 ms y retroceder 6 cuadros ~1 ms (el mapa de páginas comprometidas se rehace sólo cuando
  algún heap cambió: contador `rex::memory::HeapMutations` del SDK).
- **Herramientas:**
  - `mk9_timeline_verify` compara toda la sombra y muestra los bytes cambiados sin card;
  - `mk9_write_trace` + `tools/online/write_report.py` dicen quién escribe una página;
  - `tools/online/synctest_runs.ps1` repite el SyncTest.

## Rollback en red (F5) — en curso (2026-11-08)

`app/src/online/netplay.cpp`: lockstep en menús y cargas; en la pelea, rollback después de 120 cuadros de
aprendizaje (la intro). Sin la entrada del rival, el cuadro corre con una predicción (su última entrada); si llega
distinta, el borde del cuadro siguiente vuelve a ese cuadro y corre de nuevo los que siguen, ocultos (sin pacing ni
presentación: `sa2::renut::SetFrameHidden`). Máximo 8 cuadros de predicción, sincronía de tiempo (el lado adelantado
duerme un cuadro), y antes de un cambio de modo o de una carga se espera a que todo esté confirmado. Sólo se comparan
los hashes de los cuadros finales.

### El reposo del borde (`quiesce.h`, `rex/system/guest_park.h`)

Guardar o restaurar la memoria con otro hilo a mitad de una tarea rompe el juego: la tarea queda "pendiente" en una
memoria que nadie va a terminar. Con 40 ms de lag se colgaba en `sub_827F2D58`: un lote de raycasts asíncronos de
PhysX (`NxSceneQuery`, vtable `0x82275948`) pendiente de un cuadro al otro, cuyo evento ya se había cerrado. El borde
de cada cuadro con rollback ahora:

1. vacía el render de UE3;
2. espera los pools de hilos de UE3 (`FQueuedThreadPoolWin`: GThreadPool y los otros dos, encontrados cuando sus
   hilos devuelven trabajo, `sub_827798D8`) y recoge los lotes de raycasts pendientes (`finish(true)`, `sub_830AACC0`);
3. estaciona todos los hilos que corren código del juego (**park** del SDK): un hilo está en reposo dentro de una
   espera del kernel; mientras el park dura, el que sale de una espera queda ahí. Las esperas dentro de una tarea
   marcada (`park::WorkScope`: las tareas de los hilos de FMOD y los trabajos del job system, `sub_82778508`) no
   cuentan como reposo: el hilo sigue hasta terminarla;
4. guarda el cuadro, recibe, y si hace falta retrocede; después suelta el park.

La lista de objetos del kernel se arma antes del park (toma el lock global del SDK, que un hilo estacionado puede
tener); el SDK cuenta los cambios de la tabla de objetos (`ObjectTable::Mutations`) para no rehacerla al pedo.

SyncTest con los cuadros repetidos ocultos (`mk9_synctest_hidden`): 150-200 rollbacks de 6 cuadros, 0 diferencias,
ningún cuelgue. Costo del borde: ~9 ms, casi todo el vaciado del render (los pools y el park, menos de 0.5 ms).

### El sonido y la GPU — afuera del rollback (2026-11-08)

Con lag real, ~1000 cuadros adentro de la pelea, FMOD se colgaba: un hilo suyo recorría una lista con un índice
basura (`sub_82DEDE10`) teniendo su lock, y el juego esperaba ese lock. Restaurar la memoria de FMOD no puede quedar
consistente: debajo están XAudio2, los contextos XMA en memoria física y el decodificador XMA del host, que no se
revierten. Y aunque se pudiera, cada rollback haría sonar de nuevo los últimos cuadros (el audio "salta para atrás").

El diseño (el de los juegos con rollback): el mundo del sonido vive afuera de la simulación (`sound_memory.h`,
`mk9_sound_apart`, encendido):

- el heap de FMOD sale de un arena propio (`0x7B000000-0x7EFFFFFF`) por sus tres callbacks de memoria, y sus datos
  estáticos (`0x833A7D28-0x833AE0BC`, `0x83462198-0x834B67A0`, `tools/online/module_data.py`) no se revierten;
- XAudio2: sus bloques de `XMemAlloc` con id `0x97` (`sub_823667D8`, `sub_823667E0`) también salen del arena; sus
  datos estáticos sí se revierten (se arman al arrancar; sacarlos por rangos "adivinados" rompía al hilo de render);
- los hilos de FMOD y XAudio2 y el callback del Audio Worker siguen durante el park (`park::SetExempt`); cuando un hilo
  de FMOD entra a código del juego (los callbacks de archivos que UE3 le da a FMOD, `sub_82602B30` y compañía), es del
  juego mientras dura (`park::GuardScope`): espera a que termine el park y el park lo espera a él.

La GPU es otro mundo que avanza sola: Renut lleva en el host las cercas encoladas y completadas, pero el dispositivo
D3D del juego (en `0x40012980`, memoria de la simulación) tiene la etiqueta de cerca, la palabra de completado, los
contadores del Swap y los punteros del anillo. Revertidos, el hilo de render de UE3 esperaba una cerca que Renut ya
había completado o que nunca iba a encolar (cuelgue en `BlockOnFence`). Esas palabras quedan fuera
(`sa2::renut::GpuProgressRanges`); el estado de render del dispositivo sí se revierte (lo cachea UE3).

Resultado con 40 ms de lag + 10 ms de jitter (RTT ~115 ms): 3 de 3 peleas completas (~4000 cuadros con rollback,
185-226 rollbacks por lado, hasta 4 cuadros), sin desync, sin cuelgues, sin crashes; el sonido suena (medidor de nivel
en el reporte de audio del SDK: picos 0.07-0.45 en pelea).

Pendiente del sonido: las llamadas del juego a FMOD en un cuadro que se vuelve a correr se repiten (un golpe que suena
en esos cuadros suena dos veces). El arreglo: grabar las llamadas (66 funciones, 194 sitios) en la primera corrida de
cada cuadro y, al repetirlo, devolver lo grabado si la secuencia coincide; si diverge, llamar de verdad y parar los
eventos que la primera corrida arrancó y la nueva no.

Fluidez: los cuadros que se vuelven a correr no se dibujan (Renut descarta sus draws y la cadena de estados arranca
de cero en el siguiente; `queued.cpp`, `SkipHiddenDraw`). El contador `[fps]` separa los cuadros mostrados de los
ocultos y cuenta los mostrados tarde: con 40 ms de lag + jitter y 25-64 cuadros re-simulados cada 5 s, 60.0 fps
mostrados, el más largo 17-20 ms, 0-1 por encima de 20 ms.

Falta (F5):
- que un cuadro repetido no vuelva a disparar sonidos (lo de arriba);
- abaratar el borde de los cuadros vivos (~10 ms de vaciado del render) y copiar páginas en paralelo;
- pruebas largas con lag, jitter y pérdida; afinar la sincronía de tiempo y el delay.
