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

### Los sonidos de los cuadros que se vuelven a correr (2026-11-08)

Un cuadro que el rollback vuelve a correr arrancaba otra vez los sonidos que su primera corrida ya había arrancado (un
golpe sonando dos veces, uno o más cuadros corrido: en una pelea con 40 ms de lag, 25 de cada ~700). La fuente de
sonido de UE3 para FMOD toca un sonido (`sub_82607A48`) con una instancia de evento que saca del pool de ese sonido
(`sub_82607310`, el virtual +180 del sonido: el pool en +200/+204, en la memoria del juego) y después la arranca
(`FMOD::Event::start`, `sub_82E46DE8`). El pool se revierte con el rollback pero FMOD no: el relleno previo
(`sub_825CA9B8`) ve ocupada la instancia que la primera corrida arrancó y pone otra nueva, que se arranca y suena encima.

El arreglo (`sound_replay.h`, `mk9_sound_replay`, encendido): en una pelea con rollback se guarda, por cuadro, qué
instancia dio cada saque del pool (el sonido y el orden del saque en el cuadro) y qué instancias se arrancaron. En un
cuadro que se vuelve a correr, el mismo saque devuelve la instancia de la primera corrida y su `start` no hace nada: el
juego queda con la misma instancia que antes y el sonido sigue como iba. Lo que la corrida corregida hace distinto va
a FMOD como siempre (un sonido que ahora sí ocurre se oye tarde por los cuadros del rollback), y lo que la primera
corrida arrancó y la corregida ya no, se para con su fade (antes se le saca el callback: el del juego, `sub_82603360`,
marcaría la fuente que esté en esa dirección). Con 40 ms de lag + jitter: 68 y 37 sonidos que antes se duplicaban
siguen sonando una sola vez; 30 y 7 nuevos de la corrida corregida; sin desync.

### Partidas enteras: rounds, REMATCH, otro luchador (2026-11-08)

Las pruebas cortaban a mitad del primer round. Con partidas enteras (el host pega sin parar, el invitado pierde cada
round por KO; 30 ms de lag + 8 de jitter; `out/f3/make_rounds.py`), después de la pantalla CONTINUE? aparecieron
cuatro problemas, todos del mismo tipo: algo de afuera de la simulación tocando la memoria que el rollback restaura.

1. **Consultas de PhysX sin rastrear.** El reposo del borde recoge los lotes de raycasts pendientes de las consultas
   que vio ejecutarse, pero la lista tenía un tope de 64 y nunca soltaba las viejas: el juego crea consultas nuevas en
   cada pelea y cada round (170 en tres peleas). Una nueva sin rastrear dejaba su lote en vuelo a través del borde, y
   un rollback a ese borde dejaba al hilo de render esperando un lote que PhysX ya había corrido (`finish(true)`,
   `sub_830AACC0`, colgado). Ahora las que ya no son consultas se olvidan y la lista llena suelta la más vieja.
2. **El streaming de FMOD por la E/S asíncrona del juego.** La música y las voces se leen de bancos dentro de los
   paquetes con un archivo de UE3 (vtable `0x82072390`) cuya lectura (`sub_82604168`) encola el pedido en
   `GAsyncIOSystem` (memoria del juego) y espera un contador en la pila de FMOD. Un rollback con un pedido en la cola
   se lo llevaba: el stream quedaba ocupado para siempre y el cambio de música de CONTINUE esperaba en FMOD
   (`sub_82DE7740`). Ahora esas lecturas son del host: el paquete mapeado (`game:\Asset\...`), en el offset del bulk
   data (`sub_8305EC00`) más la posición, que el host guarda por archivo (`sound_stream.cpp`,
   `mk9_sound_stream_host`).
3. **La memoria física de FMOD y XAudio2 en el heap del juego.** Sus buffers XMA (70 KB, 67 KB y 11 KB por stream que
   se abre, pedidos desde el hilo de streams de FMOD, que corre durante el park) salían del pool físico de GMalloc, cuyo
   spin lock (`0x83424440`, dueño = id de hilo) y listas están en la memoria de la simulación. Un hilo de sonido
   tomándolo mientras se guardaba o restauraba un cuadro dejó el lock "tomado" para siempre (el hilo del juego girando
   en `sub_829AFC48`). Ahora tienen un arena físico propio de 48 MB (el tope del heap físico de 4 KB), afuera del
   rollback, como el virtual.
4. **Lo irreversible del final de la pelea.** Con el KO final empiezan las cargas (`FatalityArt` en el FINISH HIM,
   `<Luchador>_Victory`, el ícono de guardado: **guarda el perfil**, la pantalla de carga y los sonidos del menú), los
   cambios de música y el desarme de los sonidos: nada de eso se puede deshacer, y un cuadro re-ejecutado lo repetía
   (o, si se lo salteaba, rompía el resto del cuadro). Como en los otros juegos con rollback, el final va en lockstep:
   la primera carga de paquete en una pelea con rollback (`LoadPackageAsync`, `sub_82730F70`; en el combate en sí no
   hay: los X-rays vienen en el paquete del luchador) espera a que el cuadro esté confirmado y pasa el resto de la época
   a lockstep, como un cambio de modo (`netplay::AboutToLoad`); si el cuadro estaba condenado, la carga queda para la
   corrida corregida (es asíncrona: nada en el cuadro la espera). El cambio de música también espera la confirmación
   (`ConfirmFrame`).

Resultado: tres peleas seguidas con REMATCH y SELECT A NEW FIGHTER, 184, 368 y 706 rollbacks del lado del invitado,
sin cuelgues, sin desync, sin crashes; el final de cada pelea en lockstep desde el FINISH HIM.

### El borde en reposo y los fps (2026-11-08)

El `[net]` dice "at rest ~12 ms a boundary (render ~12)": casi todo es la espera del vblank. El hilo de render de UE3
hace el `Swap` y ahí espera el pacing de Renut (`[renut] game thread ... paced`: ~11 ms por cuadro en pelea), y el
vaciado del borde lo espera. El trabajo real del cuadro (juego + resto del render) es ~5 ms en esta PC: hay margen
para una PC bastante más lenta antes de perder 60 fps. El borde también deja la entrada a un cuadro de la pantalla
(`[latency] input -> present ~15 ms`): en paralelo, juego y render sumarían un cuadro de latencia. Saltear el borde en
los cuadros confirmados solo ayudaría en LAN (por internet casi todos los cuadros corren con predicción).

Fluidez: los cuadros que se vuelven a correr no se dibujan (Renut descarta sus draws y la cadena de estados arranca
de cero en el siguiente; `queued.cpp`, `SkipHiddenDraw`). El contador `[fps]` separa los cuadros mostrados de los
ocultos y cuenta los mostrados tarde: con 40 ms de lag + jitter y 25-64 cuadros re-simulados cada 5 s, 60.0 fps
mostrados, el más largo 17-20 ms, 0-1 por encima de 20 ms.

### Una sesión larga (2026-11-09)

21,5 minutos seguidos (81.839 cuadros) con 60 ms de lag + jitter + 1 % de pérdida: 11 peleas con REMATCH una tras
otra, sin desync ni cuelgues; 3.441 rollbacks del lado del invitado; la memoria del sonido quieta en 16,4 MB de
punta a punta. Los cuadros lentos caen en el arranque, en el primer cuadro de cada pelea (~100 ms: la línea de tiempo
del rollback copia la memoria al empezar, durante la presentación) y en una pelea en un escenario pesado (~430
dibujos por cuadro), con las dos instancias compartiendo la GPU integrada de esta PC: medirlo con dos PCs.

### Red mala, idiomas distintos (2026-11-08)

- **80 ms + jitter + 3 % de pérdida** (`mk9_net_lag_ms=40`, `jitter 15`, `loss 3`; ida y vuelta medida ~116 ms):
  tres peleas seguidas (la primera, REMATCH, otro luchador) sin desync; del lado del invitado 194, 382 y 430 rollbacks,
  el más profundo de 5 cuadros; las peleas a 60.0 fps. Los menús de después de cada pelea (lockstep con el delay del
  rollback, 1 cuadro) iban a **38 fps**: cada cuadro esperaba la entrada del otro (ver "El delay del lockstep").
- **Un jugador en español y el otro en inglés** (`user_language` 5 y 1; textos, arte y paquetes distintos): tres
  peleas con rollback sin desync (los registros de chequeos difieren sólo por los cuadros que el rollback repite, con
  los mismos valores). El idioma es de cada uno.

### El delay del lockstep sigue al ping (2026-11-08)

En un tramo en lockstep cada cuadro espera la entrada de ese cuadro del otro, que salió `delay` cuadros antes: los
delays de los dos lados tienen que cubrir la ida y vuelta, o cada cuadro espera (con 116 ms y delay 1, 38 fps). Cada 2 s,
y apenas termina el modo de una pelea, cada lado sube su propio delay a lo que pide la ida y vuelta
(`LockstepDelayFor`: (ida y vuelta + 25 ms) / 2 cuadros, entre el `mk9_net_delay` del host y 12). La ida y vuelta que
cuenta es el piso de los últimos 8 pings (uno cada 0,5 s): el otro contesta el ping en el borde de su cuadro, y uno
contestado después de una carga mide la carga, no la red (al arrancar, la media daba 193 ms con una red de 50-90).
Sube enseguida: los cuadros del medio repiten su última entrada (pad y "ocupado"; sin "listo" ni anuncio de sonido).
Baja tras tres chequeos seguidos (6 s): la entrada de ese cuadro ya estaba hecha y el lado no hace otra hasta que los
cuadros la alcancen. Las entradas de cada lado siguen siendo una por cuadro, así que los delays no necesitan ser
iguales en los dos lados ni hace falta acordarlos; la época de la próxima pelea vuelve al delay del rollback. En el
menú, 4-5 cuadros con 116 ms no se notan; 38 fps sí.

### Sin los logos del arranque (2026-11-09)

Cada sesión vuelve a abrir el juego, y los dos jugadores esperaban los logos de WB y NetherRealm (16 s; el título
llegaba a los ~27 s). Los logos se saltean con un botón, así que en una sesión el pad del host toca START cada 10
cuadros mientras pasa una película en sus primeros 25 s (`mk9_net_skip_logos`): el toque es una entrada como las
demás y las dos máquinas lo ven en el mismo cuadro. Cada logo dura ~23 cuadros y el título llega a los ~14 s, igual
en las dos. Las pruebas (`pair_test.ps1`) los dejan (sus guiones cuentan con ellos) salvo con `-SkipLogos`.

### Los avisos de los sonidos, al mismo cuadro en las dos (`sound_sync.h`, 2026-11-08)

FMOD llama al aviso de eventos del juego (`sub_82603360`) cuando un evento llega a un punto de sincronía (tipo 0: la
fuente recibe `+268 |= 0x40000000`) o termina (3 robado, 4 terminado: `+88 |= 0x10000000`, `+256 = -1`), desde sus
propios hilos y con el reloj de su mezclador: en cada máquina cuando le toca. En lockstep ahora esos avisos esperan:
cada máquina anuncia los suyos con sus entradas (4 bytes más por cuadro, uno a la vez y en ronda hasta que se
entrega; versión 7 del protocolo) y el aviso llega al juego en el borde del cuadro en que las entradas de las dos lo
anunciaron, el mismo en las dos porque las dos deciden con las mismas dos tiras de entradas. Las fuentes se numeran
cuando el juego le pasa su evento a FMOD en el hilo del juego (la cuenta es la misma en las dos); el final de una es un
aviso y sus puntos de sincronía los siguientes. En la pelea con rollback pasan como vienen (lo que esperaba se entrega
al empezarla). Probado: en los menús hasta la pelea, 16 fuentes y 13 avisos, entregados juntos en las dos.

**No era la causa de la desincronización rara de los menús**: con `mk9_sound_sync=false` y los avisos del invitado
90 cuadros tarde (`mk9_test_sound_late=90`), los menús llegan igual a la pelea sin diferencias. Igual queda puesto:
cierra una fuente real de diferencias (lo que el juego haga con esas marcas en otros menús o modos).

Falta (F5):
- la desincronización de los menús (época 0, lockstep) apareció **una vez** en ~15 sesiones de prueba (cuadro 3727,
  ~62 s: la confirmación de la selección y la pantalla VS); la sesión la detecta y termina con aviso. No son los
  avisos de sonido (arriba). Para cazarla: `tools/online/hunt.ps1 -Runs 10 -Extra "--mk9_net_lag_ms=25
  --mk9_net_jitter_ms=20"` repite la sesión hasta que algo se separe y guarda los registros de esa corrida (chequeos,
  sorteos, nativas de los cuadros 3700-4430);
- copiar páginas en paralelo (el `keeping a frame` de ~1.4 ms);
- una sesión de más de 15 minutos seguidos; dos PCs reales.

### El X-Ray con rollback, el detector y los sonidos en la pelea (2026-11-09)

Prueba: dos instancias con `out/f3/make_xray.py` (Versus, los dos atacan y aprietan LT+RT cada pocos golpes, así
el que tiene la barra llena tira el X-Ray) y red mala (`--mk9_net_lag_ms=45 --mk9_net_jitter_ms=25
--mk9_net_loss_pct=3`); `out/f3/xray_loop.sh N` la repite hasta que aparece un DESYNC y guarda los registros en
`out/f3/desync_run`.

1. **El detector estaba ciego en las peleas.** Cada paquete lleva el hash de un solo cuadro final, y la comparación
   se frenaba en el primer cuadro sin hash del otro (un paquete perdido, o varios cuadros que una rollback deja
   finales de golpe): con pérdida o jitter dejaba de comparar a los pocos segundos. Y con red buena no comparaba
   nada: el paquete sale antes de que el cuadro recién corrido tenga su hash, así que iba "sin hash". Ahora se saltean
   los cuadros cuyo hash el otro ya no va a mandar (`remote_hash_newest`) y se manda el último hash calculado; la línea
   de estado dice cuántos cuadros se compararon ("N frames' sync checked").
2. **Lo que encuentra:** casi siempre un script del escenario (la nativa `8248CC88`, un sorteo de `UtilsCore.cpp:2497`)
   que se despierta un cuadro antes en una máquina: los sorteos del juego salen en otro orden en ese cuadro (el chequeo
   del tick, tag 10, difiere uno o dos cuadros y vuelve a coincidir). El script espera un sonido, y el aviso de FMOD
   llega con el reloj de cada mezclador.
3. **Avisos perdidos en los cuadros que se vuelven a correr.** En la pelea los avisos pasan como vienen y marcan la
   fuente en la memoria del juego; una rollback la vuelve a antes de la marca y FMOD no avisa dos veces: en la máquina
   que volvió atrás el juego no veía terminar ese sonido. Ahora cada aviso queda con el cuadro que lo recibió y, cuando
   una rollback corre ese cuadro de nuevo, se le da otra vez al empezar el siguiente (`PassedFrameStart`; la línea de
   estado: "given again in frames run again"), si la fuente todavía toca ese evento (`+0x108` de la fuente).
4. **Probado (la versión de la release):** red local (rtt 15 ms, sin rollbacks): una pelea entera de 8320 cuadros con
   X-Rays, todos comparados, ninguna diferencia, y las capturas de las dos iguales. Red realista (30 ms por lado,
   jitter 5, 0,5 % de pérdida: rtt ~90 ms, 274/748 rollbacks): 8870 cuadros, 2 o 3 cuadros sueltos distintos que
   vuelven a coincidir, capturas iguales. Red muy mala (45 ± 25 ms, 3 %): diferencias sueltas parecidas y, en 2 de 2
   corridas largas, un cierre: el renderer leyendo vértices o índices de direcciones que no son (`ReadIndices`,
   `SwapDword`) o el hilo de render del juego con un árbol roto (`sub_82A4F710`): la memoria vuelta atrás bajo el hilo
   de render, a investigar.
5. **`mk9_net_end_on_desync` pasa a false**: lo que el detector agarra es casi siempre ese orden de sorteos, no una
   pelea distinta; con el detector andando, terminar la sesión cortaba casi todas las peleas con red mala. Queda en el
   registro ("the two games' checks differ from frame N").
6. **Experimental, apagado** (`mk9_sound_sync_fights`, `sound_sync.h` Mode::kFrames): los avisos de la pelea al mismo
   cuadro en las dos por números de cuadro (anunciados una vez con las entradas, dados en el cuadro max(L, R) en cada
   corrida, la rollback si la palabra del otro llega tarde). Funciona como mecanismo, pero no alcanza: FMOD empieza
   algunos sonidos un cuadro o dos aparte en las dos máquinas (sus instancias ocupadas y sus cargas con el reloj de
   cada una: `sub_825CA9B8` saca otra instancia por cada una que FMOD dice ocupada), así que los números de las dos no
   coinciden siempre, y un aviso que nunca se da puede dejar un script esperando. Para que el sonido de la pelea sea
   igual en las dos hay que hacer deterministas esas preguntas a FMOD (ocupado, cargado) desde el juego.

Herramientas nuevas: `mk9_synctest=0,P,K` empieza al empezar la pelea y para en la primera carga (el fin de la
pelea); con `mk9_input_script_frames` el script de mandos sigue el cuadro de la simulación, así un cuadro que se vuelve
a correr recibe los mismos botones (un synctest con golpes y X-Rays: 492 rollbacks de 7 cuadros, 3444 comparados, 0
distintos). `mk9_sound_log=<archivo>` escribe los avisos de sonido (y `<archivo>.frames`, la numeración del modo
experimental).

### Las entradas que se pisaban, las sesiones grabadas y la memoria que ya no está (2026-11-09)

1. **Dos bugs de entradas, la causa de los desyncs con red mala** (no el sonido ni los sorteos: esas diferencias eran
   la consecuencia). Encontrados con las grabaciones de abajo, comparando las entradas de las dos máquinas:
   - en la pelea con rollback, cuando una rollback caía en la misma llamada que había hecho el input del cuadro
     `latest + delay`, el cuadro en vivo volvía a ese número y el input se leía de nuevo: pisaba al que el otro ya
     tenía, y cada máquina jugaba ese cuadro con botones distintos (el X-Ray que salía en una sola). Ahora el input
     de cada cuadro se hace una sola vez (`local_made_upto`), como en lockstep;
   - `SendInputs` mandaba hasta `latest + delay`, pero ese input se hace recién en el borde siguiente: una espera
     dentro del cuadro (`ConfirmFrame`: una carga, el cambio de música de un round) reenviaba antes y mandaba un pad
     vacío, que el otro guardaba como el de ese cuadro. Ahora se manda hasta `local_made_upto` en los dos modos.
   Con 45 ± 25 ms y 3 % de pérdida, cuatro sesiones de ~4 min con X-Rays seguidos: entradas iguales cuadro por cuadro,
   ningún hash distinto, ningún cierre (antes, un desync a los 10-40 s y a veces un cierre del renderer, que venía de
   ahí: dos juegos distintos dibujando lo que el otro ya no tiene).
2. **Las sesiones grabadas** (`mk9_net_record`): cada sesión escribe sus cuadros finales en
   `logs/session_<fecha>_p1|p2.mk9net` (las entradas de los dos y el hash de sincronía de cada cuadro; quedan las 10
   últimas). `tools/online/session_report.py <p1> <p2>` dice si las entradas coinciden y en qué cuadro se separaron
   los hashes, con los botones de los cuadros de alrededor; con un archivo, sus épocas. Al detectar un desync el log
   dice dónde está el archivo, para que los jugadores lo manden.
   **Desde 2026-11-11 viene apagado** (F1 > Replays o F2 > Online lo activan). El hilo del juego nunca toca el disco:
   arma los registros de cada cuadro en un buffer y los pasa de una vez (al feed de los espectadores y a la grabación);
   con la opción activada, un hilo propio los escribe cada 2 s. Apagada, quedan en memoria (unos 3 KB por segundo de
   sesión, en pedazos de 1 MB que no se vuelven a copiar; 64 MB como máximo) sólo para el reporte de un desync, que
   los escribe a su lado (`logs/desync_.../session.mk9net`). Probado con un desync forzado
   (`mk9_net_test_desync_frame`): con la opción, las dos grabaciones se leen enteras (7801 cuadros) y el reporte
   apunta a ellas; sin ella, no se escribe ninguna y el reporte trae su `session.mk9net` (6601 cuadros, hasta el
   desync). El feed de los espectadores sube menos durante una pelea con rollback (`feed::kBudgetFight`, 17 KB/s; con
   más de 150 ms o pérdida, 8,5 KB/s): ponerse al día no le quita subida al jugador.
3. **El sonido de la pelea**: con las entradas arregladas no apareció ningún desync en las pruebas, así que los avisos
   de FMOD a destiempo no rompen la pelea (el juego no decide nada con ellos en las peleas probadas). El sonido puede
   empezar un cuadro o dos aparte en las dos máquinas; `mk9_sound_sync_fights` sigue experimental y apagado.
4. **Un cierre del renderer con red realista** (30 ms por lado, jitter 5, 0,5 %): a los 3 minutos de una sesión larga,
   en la intro de la segunda pelea, el hilo de render convirtió vértices de `0x3FC30780` (el principio del buffer), en
   la parte del heap bajo donde no hay nada mapeado: la cabecera del vertex buffer que leyó el render tenía esa
   dirección (las conversiones se hacen al mandar el anillo, mucho después de la llamada del juego). Los hashes de las
   dos máquinas fueron iguales toda la sesión: no fue un desync. Las lecturas diferidas del renderer (vértices, copias
   de fetch, índices, texturas) corren ahora marcadas (`QuietGuestFaultScope`, `guest_memory.h`) y bajo SEH: el
   manejador de fallos del SDK deja el fallo al lector (`xmemory.cpp`, sin log ni marca de crash), el draw sale con
   ceros (o se saltea) ese cuadro y queda en el log ("read from guest memory that is not there"). `MK9_TEST_GONE_READ=1`
   fuerza uno para probarlo. Cómo llega el juego a esa cabecera queda por ver; el log lo va a decir si vuelve.
   La misma sesión de 21 minutos repetida con esto: 11 peleas con rollback (hasta 2220 rollbacks por lado), 77.134
   cuadros comparados entre las grabaciones de los dos, ningún hash distinto, ningún cierre y ninguna lectura
   salteada.
5. **El desync raro de los menús, atrapado con las grabaciones** (`hunt.ps1 -Runs 8 -Dir out\pc_test`: 7 corridas
   juntas hasta la pelea, la 7.ª separada). Las entradas de las dos máquinas fueron iguales cuadro por cuadro
   (`session_report.py`): no es el netcode. Los hashes se separan en el cuadro 4222 de la época 0 (lockstep), donde un
   modo arranca un cuadro aparte (4224 y 4223). El primer rastro está antes, en el cuadro 3917: el script del invitado
   llama a la nativa `8246e498` un cuadro antes que el del host, con todas las nativas anteriores de ese cuadro iguales
   en las dos (comparadas descontando el corrimiento de 0x60000 entre los heaps de las dos). A los 3784, la nativa
   `8246f108` (`sub_82386860`: busca un objeto por nombre en la lista de `0x833CF39C`) devolvió en las dos objetos a
   0x190 de distancia, no al corrimiento del heap: el primer sospechoso. Esa corrida tuvo muchas esperas del lockstep
   (el juego a ~30 fps con la PC cargada), así que los botones del guion cayeron en otras pantallas y el camino fue
   otro: los modos arrancaron en 2350/3944/4224 y no en ~3090/~3880 con la pelea a ~4380 como en las otras siete
   (el menú de 1 jugador, no el Versus). Queda por ver qué espera de ese camino termina con el reloj de cada máquina;
   los registros están en `out/f3/desync_menu_20261109` (las dos grabaciones, los logs y las nativas, sorteos y
   chequeos de los cuadros 3700-4430).

### Lo que encontraron los testers de la 0.1.0 (2026-11-10)

Logs de un tester (Radeon RX 570; partidas Argentina-Venezuela, 140-220 ms de ida y vuelta, varias por el relevo
TURN de Cloudflare). Lo que había y lo que se hizo:

1. **Sesiones congeladas al reiniciarse el juego** (6 de 6 que empezaron "with no path yet"): por ICE el HELLO del
   invitado llega antes de que este lado tenga un par elegido para contestar (`IceLink::Send` sin camino). El host
   terminaba el handshake con cualquier palabra del otro, su HELLO se perdía y arrancaba; el invitado esperaba ese
   HELLO y el host las entradas del invitado: los dos parados para siempre. Ahora los dos esperan el HELLO del otro
   *y* un camino, y un lado que ya arrancó contesta cada HELLO que le llega (`AnswerHello`). Prueba:
   `mk9_net_late_path_ms=3000` en el host (sin camino 3 s, recibiendo): las dos arrancan y llegan a la pelea en el
   mismo cuadro.
2. **Menús trabados por el ping**: el retraso del lockstep salía del piso de los pings (131 ms con 179 de promedio):
   8 s de espera cada 10. Ahora del percentil 75 de los últimos 8 (`RoundTripTypical`), y cada época arranca con el
   retraso ya medido (antes volvía al del host y tardaba 2 s en subir).
3. **La pelea arrancaba en cámara lenta** ("se ralentizaba al iniciar la pelea"): sus primeros 120 cuadros (la intro,
   mientras la línea de tiempo aprende) van en lockstep, con el retraso de la pelea (1): ~13 cuadros por segundo con
   150 ms. Ahora la intro usa el retraso del ping, y al encender el rollback la pelea toma el suyo (`FightDelay`):
   el del host, subido con un ping largo para que un rollback vuelva unos 5 cuadros (`mk9_net_auto_delay`: 180 ms ->
   2, 220 ms -> 3, 4 como mucho). La predicción llega a 10 cuadros.
4. **Cierres en la pelea con ping alto** (`sub_82A4F710` con el árbol roto; justo antes "PhysicalHeap::Release failed",
   10 veces en sus logs): la línea de tiempo devuelve la memoria del juego pero no qué bloques tiene reservados; un
   rollback sobre las reservas y liberaciones de un X-Ray dejaba punteros a bloques liberados o liberaba dos veces.
   `heap_undo.cpp`: en una pelea con rollback las reservas del juego (memoria física, `XamAlloc`, `ExAllocatePool`;
   el SDK avisa por `rex::memory::NotifyGuestHeap`) quedan con su cuadro y sus liberaciones se aguantan hasta que
   ningún rollback llegue a ese cuadro; un rollback libera lo que reservaron los cuadros que deshace y olvida lo que
   liberaron. Con 80 ms por lado, jitter 15 y 1 % de pérdida: 21 minutos, 11 peleas, 74.919 cuadros iguales, ninguna
   "Release failed", ningún cierre (antes, cierre a los ~110 s de pelea); los rollbacks que pasan sobre esas
   operaciones son pocos (6 en 21 minutos).
5. **Bordes de 13-22 ms y hilos sin estacionar** (riesgo de desync): el hilo de streaming de FMOD copiaba música o
   voces desde un disco lento dentro del callback de archivos del juego, que el park espera (8 ms y seguía sin él).
   La copia sólo escribe el buffer de FMOD, en el arena del sonido: ahora corre exenta del park
   (`park::LendExempt` / `TakeBackExempt`) y vuelve a ser del juego antes de tocar su memoria. Con cada copia 25 ms
   más lenta (`MK9_TEST_SLOW_STREAM=25`): park de 0,01 ms, ningún hilo suelto.
6. **Parpadeos negros y polígonos que parpadean en algunos escenarios**: los cuadros que un rollback vuelve a correr
   no dibujaban pero sí hacían sus clears y resolves: texturas negras (un reflejo o captura del escenario que se
   actualiza cada tantos cuadros, un efecto que lee el cuadro anterior) leídas por el cuadro siguiente que se ve.
   Ahora esos cuadros (y los que saltea el modo de rendimiento) no hacen clears ni resolves.
7. **Desyncs de pelea en casi todas las partidas**: eran los dos bugs de entradas ya arreglados en la 0.2.0 (arriba);
   los dos desyncs de menú de sus logs se corrigieron solos en la barrera de la pelea (los dos llegaron al mismo
   cuadro), como el que atrapó `hunt.ps1`.

8. **Un DESYNC falso con ping muy alto** (~340 ms: un cuadro suelto en cada lado, las grabaciones iguales): tras un
   rollback, el hash del cuadro recién vuelto a correr seguía siendo el de su corrida anterior hasta el borde
   siguiente, y `SendInputs` lo mandaba como el del cuadro final más nuevo. Ahora un rollback borra los hashes de los
   cuadros que se vuelven a correr. Con ~340 ms: ~6000 cuadros comparados en vivo por lado, ninguno distinto.
9. **El cartel de la sesión**: en la pelea dice el retraso ("vs Rival - 180 ms - retraso 2") y, si las dos partidas
   difieren en 20 cuadros o más de la época, lo avisa y pide los `session_*.mk9net`.
10. **Rollbacks más baratos**: en el borde los hilos del juego están estacionados; la línea de tiempo reparte las
    comparaciones y copias de páginas entre hasta 3 hilos más (`PageWorkers`): guardar un cuadro 2,29 -> 1,30 ms,
    restaurar 1,49 -> 0,94 ms. Lo que más pesa en un cuadro que se vuelve a correr sigue siendo el hilo de render de
    UE3 (~3 ms): saltearlo no es seguro (UE3 marca ahí qué se dibujó, y el juego lo usa: animaciones, hitboxes).
12. **Rollbacks sin tirones** (`mk9_net_light_resim`): un cuadro que un rollback vuelve a correr con las entradas de
    los dos ya conocidas no puede salir mal predicho, así que ningún rollback va a volver a su comienzo: su borde no
    saca foto (el registro de la foto siguiente cubre los cuadros del medio con la marca del último guardado) ni
    reposa: corre como uno de lockstep, con el hilo de render de UE3 en paralelo. Reposan el borde anterior al cuadro
    que se muestra y el que deshace una predicción. Y el retraso automático sube hasta 6 con más de 300 ms. Con ~200 ms:
    299-301 cuadros mostrados cada 5 s (antes 283-291), el más largo 20-31 ms (antes 38-60); 3 % de pérdida y mucho
    jitter: 300 cuadros cada 5 s, el más largo 17-25 ms; ~350 ms: retraso 6, rollbacks de hasta 6, 297-300 cuadros
    cada 5 s, el más largo 24-35 ms (antes 50-124). Las grabaciones de los dos, iguales en todas.
11. **Probado también**: relojes de audio distintos (el invitado con `audio_sdl_pump`: 14.586 cuadros iguales), ~340
    ms con 2 % de pérdida (13.562 iguales, retraso 4, rollbacks hasta 10), el flujo entero por sala con lobby local e
    ICE con 70 ± 15 ms (sala pública, partida rápida, los dos listos, la pelea en el mismo cuadro).

Los personajes con "pixelado azul, verde, rojo" (un tester): su log de esa partida (offline, 480p, "Cuadros
dibujados: Automático") muestra el modo pasando una y otra vez a dibujar uno de cada dos. En los cuadros salteados se
hacían los clears y los resolves: un render target recién creado tiene contenido indefinido en D3D12 (ruido de colores
en AMD) y el resolve lo copiaba a texturas que el cuadro dibujado siguiente usaba: el mismo arreglo del punto 6 lo
cubre (sin clears ni resolves en un cuadro que no se dibuja), offline también.

Pendiente (menor): el costo del borde en los cuadros que se vuelven a
correr (~3 ms del hilo de render de UE3 + 2,3 ms de la línea de tiempo cada uno: un rollback de 5 cuadros es un
cuadro de 40-70 ms).

### Lo que encontraron los testers de la 0.2.0 (2026-11-10)

1. **El parpadeo, la escena negra y los triángulos de colores en las peleas online** ("la pantalla sigue
   parpadeando, como con luces verdes"). Reproducido con dos instancias y una grabación de lo que se muestra
   (`sa2_video_s`, análisis cuadro por cuadro): 31-44 cuadros cada 900 con la escena 3D negra bajo el HUD y 2-4 con
   triángulos enormes magenta, amarillos o azules, aun sin ningún rollback. La causa: el reposo de cada cuadro de una
   pelea con rollback vacía el hilo de render de UE3 (`FlushRenderingCommands`) y, desde 2026-11-07, mantenía abierta
   la compuerta de Renut (`mk9_record_after_swap`) todo ese tiempo, para no colgarse si ese hilo esperaba una cerca de
   GPU detrás de ella. Con la compuerta abierta el hilo de render tomaba los dibujos del cuadro a medida que llegaban,
   antes de que MK9 escribiera los vértices de sus partículas (los escribe después de pedir el dibujo, hasta el Swap):
   vértices viejos o en cero, o sea basura o nada (la pasada que compone la escena, la escena negra). Ahora
   (`mk9_drain_gate = watch`) el vaciado sólo vigila: si el hilo de UE3 deja de encolar trabajo 50 ms con cosas detrás
   de la compuerta, la abre (ninguna vez en las pruebas). Y el present de las esperas (el overlay, cuando el juego
   tarda más de 100 ms en mostrar un cuadro) ya no entra en medio de un cuadro: cortaba la lista de comandos y pasaba
   al slot siguiente con el resto del cuadro todavía por grabar. Quedaban destellos de otra clase, con rollbacks:
   partículas estiradas verdes y celestes y algún cuadro sobreexpuesto (las "luces verdes"): un rollback restauraba la
   memoria del juego mientras el hilo de render todavía grababa los dibujos del último cuadro mostrado, que leen sus
   vértices de la memoria del juego al grabarse (algunos viven en las páginas de 4 KB bajo 0x40000000, que la línea de
   tiempo pone de vuelta). Ahora el rollback espera ese trabajo (`render_thread::Drain`, ~4 ms por rollback, no por
   cuadro). Grabado de nuevo por ICE con 190 y 290 ms (rollbacks de hasta 7): 1500 cuadros, ninguno negro, de basura ni
   con verde; el reposo cuesta lo mismo.
2. **La resolución dinámica en 480p toda la noche**: decidía por el largo del cuadro, y un video corre a 30 por segundo
   con la GPU casi quieta: cada cinemática la bajaba a 480p, y el trabajo de 2-3 ms de una CPU rápida nunca decía que
   había lugar para subir (el jugador volvía a elegir Ultra a mano). Ahora va por el tiempo de GPU de los cuadros
   mostrados (sus timestamps): baja con más de 15 ms, sube si el nivel siguiente (por sus píxeles) tomaría menos de
   12,5 ms; mientras un video se reproduce no decide nada (tampoco el modo de dibujo automático).
3. **Una pelea con 180-585 ms de ping que se puso cada vez peor**: el retraso se elegía una vez, al final de la intro
   (184 ms: 2) y quedaba así con 300-350 ms: rollbacks de 10 cuadros, cuadros de hasta 850 ms, 27-40 por segundo. Ahora
   `AdjustFightDelay` lo revisa cada 2 s de pelea (sube de una, baja de a un cuadro tras tres chequeos; los cuadros que
   salta una subida repiten la última entrada), hasta 7. El de los menús llega a 20 (antes 12). El final de esa pelea
   ("se laggeó toda y se reinició") fue la red del jugador: Windows dijo "red inalcanzable" (10051/10065) y el lobby no
   respondía, y la sesión terminó a los 20 s sin noticias del otro, como corresponde.
4. **El ping medido con las esperas adentro**: el juego lee los datagramas en el borde del cuadro, así que un cuadro
   largo (una carga, un rollback, una GPU ocupada) se sumaba al ping (400-500 ms medidos sobre un camino de 160 ms, y
   el retraso subía a 7 medio minuto). Ahora ICE marca la llegada de cada datagrama en su hilo
   (`Transport::LastArrivalUs`), el PONG dice cuánto esperó el PING del otro lado y el que mide descuenta las dos
   esperas: con 120-180 ms emulados mide 160-170 y el retraso queda en 2.
5. **Versiones mezcladas**: la 0.1.0 y la 0.2.0 hablaban el mismo protocolo (7) con netcodes distintos. Desde la 0.2.1
   es el 8, el HELLO dice la versión, y un HELLO de otra versión termina el intento al instante con "tiene otra versión
   de mk9-lauta... actualicen los dos"; al otro le llega un BYE en el formato de su versión (la 0.2.0 deja de esperar 2
   minutos). Prueba: `mk9_net_test_protocol=7`.
6. **El audio que se fue en una fatality**: no se reprodujo (una prueba de 7 minutos con peleas enteras y rollbacks:
   el sonido sigue después de cada fin de pelea). Hipótesis a confirmar con un log: FMOD vive fuera del rollback, y
   un cambio de volumen o pausa de una categoría hecho en un cuadro que un rollback deshace no se deshace con él.
7. **Pruebas nuevas**: `mk9_net_lag_steps` ("s:ms,s:ms": el lag cambia en la sesión) y las grabaciones analizadas
   con un detector de cuadros negros y de colores basura.

### 0.3.0: espectadores, nombres, banderas y lo que se aprendió (2026-11-10)

La 0.2.1 no salió: sus arreglos van con la 0.3.0, que suma lo que pidieron los jugadores (los nombres en la pelea y en
la selección, banderas, ping por sala, silenciar, avisos, el indicador de conexión, el retraso de cada uno, el perfil,
las pantallas online en inglés y portugués, Discord) y los **espectadores**. El protocolo es el 9.

**Mirar una sala (`feed.h`).** El host de una sesión ya graba la partida para `session_report.py` (semilla, épocas,
las entradas finales de cada cuadro y su hash de sincronía). Esa grabación, tal cual, es lo que necesita un tercero
para jugar la misma partida: el host se la pasa a cada espectador por el relay del lobby (los mismos mensajes `sig` de
la sala, a la id del espectador; ningún cambio en el servidor), y el espectador arranca el juego con un perfil nuevo y
la reproduce desde el primer cuadro con las dos entradas grabadas, comparando su hash con el de los jugadores en cada
cuadro. Lo que el juego pregunta y depende del reloj de cada máquina (si terminó un video, si está lista una carga, los
avisos de los sonidos) se contesta como lo grabaron los jugadores; donde la grabación dice que una escena estaba lista,
el espectador primero espera sus propias cargas, y en los menús los callbacks de sonido llegan en los cuadros que los
jugadores acordaron, una vez que su FMOD también los tiene.

- **El cuadro de la barrera no se grababa**: cada cuadro sale a la grabación al empezar el siguiente (su hash recién
  se conoce cuando termina), y el último de una época, el de la barrera, tiene su siguiente en la otra época. El
  espectador nunca jugaba ese cuadro, así que nunca llegaba a la barrera de la pelea ("la partida pasó a la época 1 y
  esta reproducción no"). Ahora ese cuadro tiene hash propio (antes iba al primero de la época siguiente: por eso el
  protocolo 9) y se graba antes de la barrera.
- **El "desvío en el cuadro 1" era otro proceso**: al terminar la sesión del espectador el juego se vuelve a abrir, y
  con las mismas opciones de prueba pisaba los registros (`mk9_check_log` y demás). Al volver a empezar ya no se pasan
  esas opciones. Y no se repite `--user_data_root`: CLI11 rechaza una opción dada dos veces y entonces no toma ninguna
  (el espectador arrancaba sin sesión y con el lobby por defecto).
- **Ponerse al día**: más de 5 s atrás, en los menús corre los cuadros ocultos, sin pacing y sin sonido hasta quedar a
  1,5 s (~190 cuadros por segundo). En las peleas no: probado (`mk9_watch_catch_up_fights`), la reproducción se separa
  en la intro de la pelea, con otros scripts corriendo ya antes de que difieran los aleatorios: los callbacks de los
  sonidos llegan con el reloj de FMOD y algunos scripts los esperan, así que adelantar la pelea los mueve de cuadro. Es
  el mismo problema que impide sincronizar el sonido de las peleas (las respuestas "ocupado" del pool de instancias de
  FMOD, `sub_825CA9B8`): las peleas se miran a velocidad normal.
- **La sala sigue en la lista mientras juega** (si era pública, con su título: `mk9_net_room_listed`): antes la sesión
  la anunciaba privada y desaparecía, así que no había a quién mirar.
- **Ancho de banda**: ~34 KB/s de grabación repartidos entre los espectadores (11 veces el ritmo de la partida para
  uno, 3 para cuatro), máximo 4 por sala; el que deja de pedir se quita a los 20 s, y una pieza perdida (un enlace al
  lobby que se cayó) se reenvía desde lo que el espectador dice tener. La grabación que llega se lee en orden o se
  corta (un número fuera de orden dimensionaría los vectores).
- **Probado** en una PC con el lobby local: en vivo desde el primer cuadro, entrando 50 s tarde (3538 cuadros
  alcanzados en 18 s) y entrando por el botón Mirar de la lista; con peleas reales de 277 y 566 rollbacks (40 ms + 10
  de variación emulados), cero cuadros con hash distinto al de los jugadores.

**El estado del juego** está en el objeto del juego (`0x833848D4`, +4; los nombres `GS_` los da `sub_829B01D8`):
GS_INIT 0, GS_LOADING_FIGHT 1, GS_ATTRACT 2, GS_MAINMENU 3, GS_ONLINE 4, GS_KOTH 5, GS_PROFILES 6, GS_PSELECT 7 (la
selección de personajes), GS_PLAY 8, GS_GAME_OVER 9, GS_MOVIE 10, GS_FATALITY 11, GS_LADDER 12. Con él se dibujan los
nombres en la selección; el log de la sesión dice cada cambio.

### El sonido de la pelea, igual en las dos, y el desync raro de los menús (2026-11-10)

**El sonido de la pelea por números de cuadro, ahora por defecto** (`mk9_sound_sync_fights`, `sound_sync.h` kFrames).
El modo existía apagado porque no alcanzaba: las dos máquinas no entregaban los mismos sonidos en los mismos cuadros, y
duplicaba los rollbacks. Medido con dos peleas con golpes reales y 40 ± 10 ms (`mk9_sound_log` con la pila de cada
entrega):

1. **Las instancias de FMOD se piden en modo bloqueante en una sesión** (`mk9_sound_blocking`, el hook de
   `sub_825CA9B8`). En modo no bloqueante FMOD devolvía la instancia mientras sus datos todavía cargaban y el juego la
   entregaba en el cuadro en que terminaba la carga, en cada máquina con su disco: 1139 de 2322 cuadros con entregas
   distintas. Bloqueante: 0 en la primera pelea, 2 en la segunda. Los cuadros de la pelea siguen a 60 (el peor, 21 ms);
   las esperas largas son de las cargas de los menús, como antes.
2. **Un aviso va al juego 8 cuadros después de la segunda palabra** (`kSlack`): la palabra de la otra máquina llega
   con una entrada hecha antes de su cuadro, así que casi siempre antes de ese cuadro. Antes iba en `max(L, R)` y la
   palabra tardía obligaba a volver atrás: ~360 rollbacks más por pelea (el doble). Ahora 0 por sonido.
3. **Un aviso que dijo una sola máquina va igual**, 45 cuadros después de su palabra (`kAlone`), decidido igual en las
   dos con las entradas del otro hasta ahí (`Decide`): la máquina cuyo FMOD no lo dio corta ese sonido y le da el aviso
   al juego ella misma, desde la entrega (`g_by_number`); el aviso propio que llegue después se descarta. Antes esos
   avisos (~4 %) no llegaban nunca a ninguna de las dos: un sonido que el juego no veía terminar.
4. Probado: peleas enteras con 40 ± 10 ms y 60 ± 40 ms, ningún cuadro con chequeos distintos.

**Un espectador** entrega los avisos de la pelea según las palabras grabadas de los dos jugadores (el bit 1 del byte de
rollback del registro 'E' dice que la época iba por cuadros), y los que su FMOD no dio los da desde la fuente; en los
menús igual (`WatchDeliver`; antes esperaba hasta 4 s a su propio FMOD, y con un sonido largo se pasaba).

**El desync raro de los menús: dos esperas con el reloj de cada máquina.** Se encontraron haciendo que un espectador
adelante los menús (sus cuadros rápidos agrandan cualquier dependencia del reloj; un espectador que entraba tarde
llegaba al menú principal 209 cuadros después que los jugadores) y comparando sus nativas con las del host:

1. **La nativa `824b75f8` es `appSeconds()`**: el reloj de la PC (`sub_82366BF8` por `GSecondsPerCycle`), que los
   scripts de los menús usan para sus tiempos. En un juego de sesión ahora contesta el contador de cuadros del juego /
   60 (`[0x833848D4]+108`, el tag 9 de los chequeos: igual en las dos máquinas y en un espectador, y vuelve atrás con un
   rollback; `mk9_script_clock`), desde el primer cuadro del juego y no desde que la sesión empieza: con el reloj de
   la PC hasta ahí, un tiempo que un script guardó antes quedaba mezclado con el de los cuadros y la espera de la
   pantalla VS terminaba un cuadro aparte en las dos máquinas (3 de 7 cacerías con red mala; con el reloj desde el
   primer cuadro, 0 de 7).
2. **La nativa `8247b658`** pregunta si terminó la tarea del motor en `0x8341EF1C` (creada por `FEngineLoop::PreInit`,
   vtable `0x82001E58`; al tocar START en el título: el perfil leído del almacenamiento) y, si terminó, la cierra (+20).
   Ahora la pregunta va sin cerrarla (+16), las dos máquinas se ponen de acuerdo (`kReadyTask`, `netplay.h` Ready) y la
   nativa (que la cierra) corre en ese cuadro en las dos; un espectador cuya tarea no terminó la espera ahí.

Con las dos, un espectador que entra 95 s tarde adelanta los menús y llega al menú principal y a la selección en los
mismos cuadros que los jugadores, y sigue la sesión entera (tres peleas) llegando a cada barrera junto a ellos.
Las dos dependían del reloj también entre jugadores: con la PC cargada (las esperas del lockstep) los tiempos de los
scripts caían en cuadros distintos; es lo que separó la corrida de `out/f3/desync_menu_20261109`.

**Lo que falta**: adelantar las peleas para un espectador. Con los avisos por cuadro su sonido ya no depende del reloj,
pero a los ~550 cuadros de la pelea adelantada los luchadores quedan en otro lugar que el de los jugadores (dos
espectadores del mismo partido, uno en vivo y otro adelantando, difieren desde el cuadro ~518; `mk9_watch_catch_up_fights`
lo prueba). Mientras tanto las peleas, y la carga que las precede (GS_LOADING_FIGHT: lo que el motor carga toma el
tiempo real de los cuadros de los jugadores), se miran a velocidad normal; los menús y lo que sigue a cada pelea, rápido.

Herramientas: `mk9_net_input_replay=<grabación>` (los botones de un jugador desde su `session_*.mk9net`, cuadro por
cuadro: una sesión otra vez por el mismo camino; con `mk9_seed` igual al de la grabación), el log de cada cambio del
estado del juego (`GS_`), y los cuadros que un espectador comparó y difirieron ("N of M frames so far").

### El reloj del hilo del juego, hecho de cuadros (`virtual_time.h`, 2026-11-11)

Todas las lecturas del reloj del invitado pasan por `REX_QUERY_TIMEBASE()` = `Clock::QueryGuestTickCount()` del SDK
(`mftb`); las del hilo del juego, por `appCycles` de UE3 (`sub_82366BF8`, muchas veces en línea). Cada máquina tiene
su reloj, así que cualquier cosa que el juego haga con el tiempo puede caer en cuadros distintos en las dos: ya
aparecieron dos (la nativa `824b75f8` y la tarea del motor, arriba). Ahora, en un juego de sesión (jugador o
espectador; `mk9_virtual_time`), el hilo del juego lee un reloj hecho de sus cuadros, desde la primera instrucción de
`main` (`sub_823669D8`): un solo reloj todo el tiempo. Los demás hilos (render de UE3, FMOD, XAudio2, streaming)
siguen con el de la PC. El SDK tiene un filtro (`rex/chrono/guest_tick_filter.h`, `SetGuestTickFilter`) que
`QueryGuestTickCount` aplica si está puesto; el filtro es de la app y decide por hilo.

**Por qué no el de Tekken 6** (`t6_determinism.cpp`: +10 µs por lectura). Se auditó antes de prender
(`mk9_clock_audit`: en cada cuadro, las lecturas del cuadro anterior por llamador, con la pila del host;
`tools/online/dev/clock_audit.py` compara dos máquinas). En ~12.000 cuadros de una sesión con 45 ms por lado, la
cantidad de lecturas de la lógica de un cuadro difería entre las dos máquinas en 7766:

1. Las esperas al hilo de render de UE3 (`sub_82741630`, la cerca de comandos del tick; `sub_82360480`, que mientras
   espera hace carga asíncrona en tajadas de 10 ms) leen el reloj sólo cuando esperan: hasta 20.017 lecturas en un
   cuadro.
2. La primera pasada de la purga incremental de basura (`sub_82568540`, llamada por `IncrementalPurgeGarbage`
   `sub_826D3CD8` con su presupuesto de tiempo) pregunta a cada objeto si el render ya lo soltó (vtable +44) y vuelve a
   preguntarle al mismo hasta que sí, leyendo el reloj cada 10 preguntas: cuánto gira depende del render.
3. La zona de PhysX lee una vez por cuadro sólo cuando su hilo viene atrasado (`sub_8301AF20`, `sub_8301AF80`,
   `sub_8312B940`, …: presente en una máquina y no en la otra en ~4500 cuadros).

Con +10 µs por lectura, las dos máquinas leerían horas distintas.

**El modelo**: cada lectura de un cuadro da la misma hora, `1000 s + cuadro × 1/60`, con `cuadro` = el contador de
`GuardedMain` (`[0x833848D4]+108`: memoria del juego, igual en las dos y vuelve atrás con un rollback, así que un
cuadro re-corrido lee la hora de su primera corrida). Que la cantidad de lecturas difiera no importa. Las esperas a otro
hilo (`WaitZone`: las dos de la cerca y la pasada de la purga) avanzan 10 µs por lectura desde la hora del cuadro (sus
presupuestos y las tajadas de la carga terminan como con el reloj de la PC) y después vuelve la hora del cuadro. Válvula:
más de 20.000 lecturas fuera de las esperas en un cuadro (lo más visto con el modelo: 128) avanzan 10 µs por lectura,
con aviso en el log. Cada 600 cuadros el log dice cuántas lecturas hace el hilo del juego (`[time]`). El reloj de los
scripts (`mk9_script_clock`) se hace a un lado cuando esto está prendido: la nativa contesta `appSeconds` de este mismo
reloj.

**Lo que el reloj no arregla**: qué destruye la purga en cada cuadro sigue dependiendo del render (es lo que corre los
heaps de las dos máquinas, el corrimiento de `0x60000` visto antes); el avance de la carga asíncrona depende del disco
(las cargas ya son barreras). Ninguno de los dos llega a la lógica de la pelea.

**Probado** (prendido por defecto): `hunt.ps1 -Runs 6` con 60 ± 40 ms, las 6 juntas (la pelea y los modos empiezan en
el mismo cuadro en las dos, ningún desync); dos sesiones auditadas, ningún cuadro pasó la válvula: lo más fuera de las
esperas, 128 lecturas en un cuadro, y hasta 212.744 dentro de ellas. Antes de meter la pasada de la purga en una espera
llegaba a 17.162 en una sola máquina (la otra, 2): la válvula se habría abierto de un solo lado. Los dos juegos de una
sesión tienen que tener el mismo valor de `mk9_virtual_time`; desde el protocolo 10 el HELLO lo dice y el log avisa si
difiere.

### El espectador que adelanta una pelea: la posición de los sonidos (tarea A5, 2026-11-11)

Un espectador que entraba tarde se desviaba de la partida a los ~430-560 cuadros de una pelea adelantada (sus cuadros
ocultos, rápidos y en silencio), así que las peleas se miraban a velocidad normal. Los experimentos del plan, con
`two_watchers.ps1` (uno en vivo, otro 95 s tarde) y `fight_watch.ps1`:

1. **Con el reloj de cuadros prendido** (A1) se desviaba igual (cuadro 557): no era el reloj del hilo del juego.
2. **Oculto pero al ritmo de los jugadores** (`mk9_watch_catch_up_mode=paced`): 5461 cuadros de pelea ocultos y en
   silencio sin ninguna diferencia. No era no dibujar ni el silencio: era ir **más rápido que el tiempo real**.
3. **La causa** (comparando las nativas de los dos espectadores, `mk9_native_log_frames`): la nativa `824a5610` le
   contesta a un script del escenario cuántos cuadros le faltan a un sonido: `sub_82A31410` busca entre los componentes
   de audio del dispositivo (`+640`) el que tiene el id del script (`+168`) y `sub_82A33C00` hace (largo − posición) ×
   60 con sus virtuales `+336` (`sub_82604DD8`: `lengthms` de `FMOD::Event::getInfo`) y `+300` (`sub_82589898`:
   `positionms`, guardada también en el componente `+156`). `positionms` es dónde va el evento en el mezclador de FMOD,
   con su reloj: con los cuadros más rápidos que el mezclador le faltaban 140 cuadros donde a los jugadores 77, y el
   script siguió por otro lado (un sorteo de más, `UtilsCore.cpp:2497`, unos cuadros después).
4. **El arreglo**: con el reloj de cuadros, el `getInfo` del hilo del juego (`sub_82E46D40`) contesta como posición
   el tiempo desde que el juego empezó ese evento, en cuadros (el cuadro de su `FMOD::Event::start`, `sub_82E46DE8`,
   anotado en el hook de `sound_replay.cpp`), como mucho su largo. También saca a los jugadores de una dependencia del
   reloj de cada mezclador (un cuadro de diferencia al cruzar el umbral del script). El log `[time]` cuenta cuántas
   posiciones contestó así.
5. **Resultado**: el espectador tarde adelanta la pelea entera (4866 cuadros en ~15 s, ~475 cuadros por segundo) sin
   ninguna diferencia con la partida y llega a las barreras siguientes en los mismos cuadros que el que mira en vivo
   (dos corridas). `mk9_watch_catch_up_fights` queda prendido: un espectador se pone al día también en las peleas (no
   mientras la pelea carga: lo que el motor trae del disco tarda el tiempo real de los cuadros de los jugadores).

Quedan como herramientas `mk9_watch_catch_up_mode` (`drawn`: dibujados y presentados sin ritmo, con
`renut_vsync=false`; `paced`: ocultos al ritmo de los jugadores) y el log de sonido de cada espectador
(`out\f3\<nombre>_sound.txt`).

**Visto de paso (a revisar):** cuando dos sonidos empiezan en el mismo cuadro, las dos instancias pueden dárselos a FMOD
en distinto orden (el cuadro 35 de la pelea: los eventos `05C0` y `05C8` con números `2301`/`2302` cruzados), así que
la clave de un aviso (cuadro + lugar en el cuadro, `sound_sync.h`) puede nombrar sonidos distintos en dos máquinas.
Ahí no tuvo efecto (los dos terminaron el mismo sonido), pero es un riesgo entre jugadores: numerar por algo que no
dependa del orden (el evento de FMOD, el componente de audio) en vez del lugar.

### El hash de la pelea con el estado de los luchadores (tarea A2, 2026-11-11, protocolo 10)

El hash de cada cuadro eran los chequeos del propio juego (sus contadores y un sorteo de su generador): un desync se
veía recién cuando llegaba al generador de azar. Ahora, en `GS_PLAY`, entra también el estado de los luchadores
(`fight_state.h` `HashFighters`): de cada jugador, tramos del bloque de la info de la partida (`0x833C6C54`, `0xC8C`
por jugador), del objeto del luchador (`[+0xDF4]`) y de su actor (`[objeto+8]`), leídos una vez por cuadro y
hasheados desde la copia.

**Qué tramos** (`app/src/online/fight_hash_ranges.inc`, generado): las palabras que en dos sesiones enteras (40 ± 10 y
80 ± 30 ms, dos peleas cada una, una muestra cada 10 cuadros en las dos máquinas: `mk9_fighter_dump` +
`tools/online/dev/fighter_diff.py`) fueron **iguales en las dos máquinas en todas las muestras y cambiaron** durante la
pelea: 357 palabras de la info, 214 del objeto (la vida en `+0x78C0` entre ellas) y 13 del actor (la posición en
`+0xDC`: `+0xE0` el eje de la pelea, `+0xE4` la altura): 345 tramos, ~2,3 KB por jugador. Afuera: lo que difiere entre
las máquinas (punteros: los heaps no coinciden; en la segunda sesión, decenas de palabras con ids `0x1000xxxx` donde el
invitado iba **2 adelante** del host en la segunda pelea: algún contador global de ids corrió dos veces más en una
máquina entre las peleas, sin que los chequeos del juego lo vieran: una pista para el desync raro de los menús) y lo
que nunca cambió (no detecta nada y una palabra quieta en estas peleas puede no estarlo en otras).

**Probado**: `mk9_net_test_desync_frame=4700` (el invitado le saca un 5 % de vida a su luchador justo antes de ese
cuadro, en cada corrida): las dos máquinas dicen `DESYNC in epoch 1 at frame 321 (game frame 4700)`, ese mismo cuadro.
Antes de A2 ese desync no lo veía nadie hasta que tocara el generador.

**Falsos positivos encontrados con el reporte de A3** (los primeros dos días de uso): en los primeros cuadros de una
pelea, dos floats que todas las muestras tenían iguales difirieron en el momento del hash en 2 de ~10 sesiones: la info
de la partida `+0x118` (`7.20164` contra `7.01414`; `+0x114` es una vtable, `0x82003cb0`) y el objeto del luchador
`+0x240` (`7.3794` contra `7.37928`), ~7 s, lo que tardó la carga de la pelea, escritos una vez cuando una carga
termina y con el reloj real (el flush del borde puede terminar una). Los chequeos del propio juego siguieron iguales en
los 5279 cuadros de esa sesión: no son el estado de la pelea. Quedan afuera con su motivo (`fighter_diff.py`
`EXCLUDED`), igual que el actor `+0x1b4` (un entero que difirió en un cuadro de otra sesión) y el actor `+0x10`
(`220.166` contra `218.166`, exactamente 2 de diferencia, en el primer cuadro de la pelea). El patrón de todos los de
los primeros cuadros: un valor que se escribe una vez cuando la pelea termina de cargar (un tiempo de carga, uno de
creación, una cuenta de lo que la carga hizo) y que las dos máquinas no escriben en el mismo cuadro. Por eso la regla
ahora es que una palabra del hash tiene que **cambiar también en la pelea** (más de 120 cuadros después de su
comienzo), no sólo al empezar (`fighter_diff.py`, `LATE`; `--within` filtra una tabla anterior sin perder lo que sus
sesiones excluyeron): 336 tramos, 569 palabras por jugador. También aparece, a veces, un racimo de campos del objeto
y del actor (`+0x38`, `+0x44` que difiere exactamente 12.0, `+0xfc`, el actor `+0x60` y `+0x13c`…) que se separa
para siempre a mitad de una pelea sin que cambien los chequeos del juego: afuera también.

**El SyncTest ahora compara los luchadores enteros** (`SyncTestFighters`: después de cada cuadro, con el borde en
reposo, el bloque de la info, el objeto y el actor de los dos jugadores contra la primera corrida de ese cuadro,
palabra por palabra). En 2844 cuadros re-corridos (475 rollbacks de 6) la info y el objeto volvieron **siempre**
iguales y los chequeos del juego también; sólo cuatro palabras del actor no: `+0xc8` (604 veces: `187.152` la primera
corrida, `187.037` la re-corrida, 0,115 s = los 7 cuadros de la rollback: la hora del último dibujo), `+0x13c` y
`+0x68` (otras horas) y `+0x1b4` (una cuenta, 15 contra 14). Son lo que el dibujo le devuelve al juego: un cuadro
re-corrido no se dibuja, y las dos máquinas re-corren cuadros distintos; de ahí el `+0x1b4` y la parte del actor del
racimo. Quedan en `EXCLUDED` con ese motivo (ya estaban afuera). La parte del objeto del racimo no es de la rollback
(el objeto vuelve igual): es algo de cada máquina con su reloj o su carga. Para B2 (no mandar la escena en los cuadros
re-corridos): estas cuatro ya dependen del dibujo y no cambian la pelea; el SyncTest con los luchadores es la prueba.
El volcado ahora guarda
también el momento del hash (tipos 3-5): una palabra que cambie entre el comienzo del borde y el hash en una máquina
(otro hilo, el trabajo del propio borde) queda afuera, y `intro_hunt.ps1` vuelca cada cuadro de los primeros segundos
de la pelea, donde terminan esas cargas.

**El protocolo 10**: el hash cambió, así que dos versiones no se pueden juntar (el HELLO ya lo dice y la sesión termina
con "otra versión"). El HELLO lleva además un byte de ajustes (bit 0: el reloj de cuadros). La grabación lleva el
protocolo en su versión (`1 + protocolo × 256`) y un espectador de otra versión se va con su motivo en pantalla.

### El reporte de un desync (tarea A3, 2026-11-11)

El primer cuadro cuyos hashes difieren deja, en cada máquina, `logs\desync_<fecha>_p<N>\`: `fighters.bin` (los bytes
hasheados de los luchadores de los últimos cuadros hasta ese: `HashFighters` guarda los de los últimos 64, la última
corrida de cada uno, y `WriteFighters` los escribe con la tabla de tramos) y `summary.txt` (versión, protocolo, cuadro
de la época y del juego, los dos hashes, retraso, ida y vuelta, rollbacks, y qué `.mk9net` tiene las entradas de la
sesión). La ventana de fin de sesión dice que hubo un desync y tiene un botón que abre la carpeta. Desde que el hash
de los luchadores es un diagnóstico (más abajo), su primera diferencia con el hash de sincronía igual deja otro
reporte, `desync_<fecha>_p<N>_fighters\`, que la ventana no ofrece.
`tools/online/desync_report.py <carpeta p1> <carpeta p2>` dice el primer cuadro y el primer campo distinto (nombrando
la vida y la posición), con los dos valores en hexadecimal y como float; si los luchadores son iguales, la diferencia
está en los chequeos del juego (sus sorteos: `rand_report.py`).

### Lo que el hilo del juego le pregunta al sistema (tarea A4, 2026-11-11)

El tiempo: el reloj del invitado (`mftb`) está hecho de cuadros (A1) y la posición de los eventos de FMOD también (A5).
`KeQueryPerformanceFrequency` es constante. Queda la hora de pared, `KeQuerySystemTime` (la del host), que el juego lee
desde 7 funciones (`tools/online/import_callers.py`): la capa de plataforma de UE3 (`sub_82373E60`, `sub_82374208`,
`sub_82374378`, `sub_82376E98`, `sub_8237AD50`) y el CRT (`sub_82BDAE88`, `sub_82BDD918`). Un hook en cada una cuenta
las del hilo del juego en una sesión: el log avisa con la pila la primera vez que una aparece y la línea `[time]` dice
cuántas van. No se la reemplazó por una hora hecha de cuadros: el SDK la usa también para los temporizadores del
kernel (`xtimer.cpp`, en otro hilo), y una fecha absoluta corrida entre hilos los haría saltar. El perfil y el
almacenamiento ya arrancan nuevos en cada sesión, el idioma se probó mezclado, la red del juego no se usa.

### Regresión de un comando (tarea A6, 2026-11-11)

`tools/online/regress.ps1 [-Quick]`: el SyncTest (con el reloj de cuadros, que ahora también se prende con
`mk9_synctest`), `hunt.ps1` con 60 ± 40 ms y 2 % de pérdida (4 corridas), un espectador que entra a los 95 s y se pone
al día en el menú y en la pelea, y una sala completa por el lobby local. Sale con error ante un `DESYNC`, un espectador
que se aparta de la partida o no llega a la pelea, una sala cuya pelea no empieza o un volcado de cierre. La cacería
ahora compara los modos por el contador de cuadros del propio juego (el tag 9 de los chequeos): la primera columna del
registro cuenta los bordes, cuadros re-corridos por la rollback incluidos, y después de una pelea las dos máquinas
tenían números distintos para el mismo cuadro.

### La fase de las dos grillas (tareas C1/C2, 2026-11-11: el mecanismo, apagado hasta medirlo a 60 fps)

Cada máquina larga sus cuadros en una grilla de 60 Hz con su propio reloj (`sa2_renut_pacing.cpp`; en la PC no hay
sincronía con el refresco del monitor), con cualquier fase entre las dos. Con ~44 ms por lado, 8 ms de fase hacen que
un lado vea las entradas del otro 2,2 cuadros tarde y el otro 3,1, y uno solo hace casi todas las rollbacks (la línea
de base: 0 contra 305). El time sync de antes iba por cuadros enteros (la ventaja con medio ida y vuelta, y un cuadro
de sueño por segundo con 2 o más), y no ve una fase de pocos ms.

**La medida** (`NotePhase`): cada paquete de entradas lleva el cuadro vivo que empezó la última liberación de la grilla
del que lo manda, cuánto pasó desde ella y su propia medida. El que lo recibe ubica cuándo empezó ese cuadro allá (la
llegada menos medio ida y vuelta menos lo que pasó) contra cuándo empezó acá (guarda el comienzo de sus últimos 128
cuadros vivos): por cuadro, no por 1/60 s, así una PC atrasada en su grilla no la tuerce. Medio ida y vuelta es una
suposición y el jitter suma de los dos lados: el error es el mismo en las dos medidas, así que la fase es la mitad de
su diferencia (como en NTP). Probado: las dos medidas salen opuestas (−143,5 contra +141,8 ms; −178,6 contra +178,6).

**El ajuste** (`mk9_net_phase_align`, apagado): con las dos medidas, el lado adelantado 2 cuadros o más duerme un
cuadro (a lo sumo uno por segundo, como antes pero con la medida buena: en una PC cargada el "ida y vuelta" llevaba
las esperas de sus cuadros y el lado adelantado 100 ms no dormía), y uno adelantado 2 ms o más en tres chequeos
seguidos corre su grilla un cuarto de eso, 2 ms como mucho, una vez por segundo (`ShiftGrid`); el de atrás no hace
nada. Sin las medidas (los primeros segundos), como antes.

**Lo que falta**: medirlo a 60 fps (`tools/online/dev/phase_ab.ps1`: la misma sesión con y sin, las rollbacks de cada
lado). Las pruebas de hoy corrieron a 20-30 fps porque la PC tenía otro juego abierto: así no se puede juzgar, y una
primera versión (la mitad de la fase por segundo, sin banda muerta) hizo que los dos lados corrieran sus grillas de un
lado a otro (101 y 133 ms en 100 s). Con el ajuste apagado la medida corre igual (el log `[net]` dice "phase X ms ahead
(here, there)"): en una cacería, el invitado iba ~10 ms adelante.

### Replays (tarea F5, 2026-11-11)

Un replay es un espectador cuya partida es un archivo (`mk9_net_replay=<logs\session_*.mk9net>`): `feed::LoadFile` lo
pone entero en el búfer que el relay llenaría, y el resto es el reproductor del espectador (sus hashes contra los
grabados incluidos). La grabación del invitado tiene su propia entrada primero: con el jugador de la cabecera en 1 se
cruzan. El ritmo: los menús rápido y en silencio, las peleas a su ritmo (no lo que falta para el vivo: no hay vivo); la
carga de la pelea, como siempre, a velocidad normal. El juego se vuelve a abrir con un perfil nuevo, como el de un
espectador (`session::Replay`), desde **F1 > Replays** (las grabaciones por fecha, una por sesión, con el rival de las
partidas del perfil que terminaron en ella). Probado con la grabación del host y la del invitado de la misma sesión:
8922 y 8859 cuadros comparados, ninguno distinto. Una grabación de otra versión (su protocolo en la cabecera) se
rechaza con su motivo.

### Avisos de la sesión y el cartel en la pelea (tareas C4, C5 y F4, 2026-11-11)

- **Menús lentos** (C4): en lockstep con 8 cuadros de retraso o más (~130 ms de ida y vuelta), el cartel dice "Conexión
  lenta: los menús responden con retraso": los menús esperan los botones del otro, el juego no se trabó.
- **Sin noticias del otro** (C5): `SessionInfo` lleva los segundos desde la última palabra del otro y los del corte
  (`mk9_net_timeout_s`, 20). A los 10 s un aviso en el medio de la pantalla dice cuántos quedan antes de que la sesión
  termine (el log lo anota una vez). Probado matando al invitado: aviso a los 10 s, fin a los 20.
- **El cartel en la pelea** (F4, `mk9_online_badge`: F2 > Online): completo, sólo la señal u oculto; los avisos
  (conexión inestable, esperando al otro, desync) se ven siempre, y en los menús completo.
- Los tres se prueban con `tools/online/dev/silent_peer.ps1` (una sala del lobby local: el cartel es de las sesiones de
  sala, no de las de UDP directo).

**El hash de la pelea, otra vez** (la segunda regresión): la info de cada jugador tiene de `+0x24` a `+0x238` un arreglo
de entradas de 12 bytes {vtable `0x82003cb0`, un tiempo ~7,01 s, un id `0x1000xxxx`} cuyos tiempos salieron 0,1875 s
distintos en el primer cuadro de una pelea (`+0x148`, después de `+0x118`) y cuyas entradas se mueven: el rango entero
queda afuera (`fighter_diff.py` `EXCLUDED_RANGES`), 324 tramos.

### El hash de los luchadores pasa a ser un diagnóstico (2026-11-11, protocolo 10)

La tercera regresión volvió a fallar por lo mismo: la info `+0x260`, `7.01256` contra `7.20006` (otra vez 0,1875 s),
en el cuadro 828 de la pelea, con los chequeos del juego iguales. Desde `+0x25c` la info tiene otro arreglo, de
entradas de `0x2C` bytes {vtable `0x820f3b60`, un tiempo, un id `0x1000xxxx`, …}, y el objeto del luchador tiene más
entradas así. Son objetos que anotan **cuándo terminó una carga suya**: esa carga corre en otro hilo y termina cuando
lo deciden el disco y el hilo de render de cada máquina, y el hilo del juego la ve terminada en su espera del render
(donde el reloj de cuadros avanza por lectura). Ningún reloj hace eso determinista, y en ninguno de estos casos
divergieron los chequeos del propio juego.

Dos cambios:

- **Por estructura, no de a uno**: la palabra que sigue a una de las cinco vtables que llevan esa hora (`0x82003cb0`,
  `0x820f3b60`, `0x820124b8`, `0x8200ff28`, `0x82010028`), en cualquier bloque, queda afuera (`fighter_diff.py`
  `TIMED_VTABLES`). Eran 56 palabras hasheadas, una por entrada del arreglo. Quedan 268 tramos, 1992 bytes por jugador.
- **El hash que avisa: los chequeos del juego, la vida y la posición.** El hash de cada cuadro son los chequeos (y los
  `mk9_net_sync_ranges`) más, en la pelea, la vida (objeto `+0x78C0`, 8 bytes) y la posición (actor `+0xDC`, 12
  bytes) de cada jugador (`fight_state.h` `HashCore`): lo que las dos pantallas muestran. Esos campos nunca difirieron
  entre las máquinas en 7494 muestras de dos sesiones, y siempre estuvieron en las dos. Hace falta: con
  `mk9_net_test_desync_frame` (un 5 % de vida menos en una máquina) los chequeos del juego **nunca** se separaron, y las
  barras de vida estuvieron distintas 359 cuadros. Ahora es un desync en ese mismo cuadro (`DESYNC in epoch 1 at frame
  316 (game frame 4700)`). Esto es lo que avisa a los jugadores (20 cuadros distintos), puede terminar la sesión
  (`mk9_net_end_on_desync`) y deja el reporte de A3 que ofrece la ventana de fin. La grabación y el espectador lo
  comparan igual.
- **El resto de los luchadores, un diagnóstico.** La tabla entera va aparte en el mismo paquete (`INPUT` termina con
  un u64, el de ese mismo cuadro final; `CompareFighters`). Si difiere con el hash de sincronía igual, lo anota una vez
  por época ("the fighters' state differs from frame …") y deja un reporte `logs\desync_<fecha>_p<N>_fighters\` (el
  `summary.txt` dice qué difirió), sin avisarle a nadie. Así sigue diciendo el cuadro exacto donde algo se separa, y sus
  falsos positivos ya no molestan a los jugadores. El espectador no lo calcula (adelanta con otros tiempos de carga que
  los jugadores). `hunt.ps1` y `regress.ps1` cuentan esas líneas aparte, sin fallar (y buscan "DESYNC in" distinguiendo
  mayúsculas: `Select-String` no lo hace solo, y encontraba "desync" en la carpeta de un reporte).

### Los cuadros re-corridos sin dibujar la escena (tareas B1 y B2, 2026-11-11: el mecanismo, apagado hasta el hunt)

**B1, la medida**: `[frame]` agrega una segunda línea con los cuadros que el rollback corre otra vez (los ocultos):
su trabajo, sus esperas al render y el borde que sigue a cada uno (el que vacía el trabajo del hilo de render de ese
cuadro y guarda su foto; un cuadro oculto no espera el vblank, así que ese borde es trabajo puro).

**B2**: `FViewport::Draw` es `sub_828BBEC0`. Se encontró por los nombres de sus comandos de render: los
`DescribeCommand` `sub_828A9170` y `sub_828A9180` devuelven "BeginDrawingCommand" y "EndDrawingCommand" (0x82133848,
0x8213385C); sus vtables de 3 entradas {destructor, Execute, DescribeCommand} están en 0x82133B90 y 0x82133B9C/0x82133BCC
y las construyen `sub_828BBEC0` y `sub_828BB750`. Encola el comienzo del dibujo, llama al `Draw` del cliente del viewport
(la escena y el HUD, vtable +12) y encola el final (el Present: el Swap que cuenta `g_mk9_game_frames`). La llaman los
redibujados de `UGameEngine` (`sub_82869CA0`, `sub_82869E20`; `sub_82875358` salta a ella). Con
`mk9_hidden_frames_draw = false`, un cuadro que el rollback corre otra vez (de un jugador o del SyncTest) no la llama:
el hilo de render de UE3 no tiene escena que recorrer y Renut nada que tirar. Un espectador o un replay que alcanza
sigue dibujando: corre oculto muchos segundos seguidos, y lo que el juego haga con "dibujado hace mucho" (un esqueleto
que sólo se actualiza si se ve...) no es lo que hicieron las máquinas de los jugadores. Hook nuevo: el generador llama
por el nombre a lo enganchado sólo tras `python tools/direct_calls.py --redo` (reescribe los 3 archivos que la llaman).

Medido (SyncTest 4700,12,6, 325 rollbacks de 6 cuadros cada uno, con la PC más tranquila):

| cuadro re-corrido | trabajo del juego | borde después (vacía su render) | total |
|---|---|---|---|
| dibujando (como hasta ahora) | 1,85 ms | 1,39 ms (p99 3,56) | 3,24 ms |
| sin dibujar (`mk9_hidden_frames_draw=false`) | 1,76 ms | 0,44 ms (p99 0,80) | 2,20 ms |

El SyncTest: 0 cuadros distintos en las dos maneras (más de 600 rollbacks en total). De los bloques de los luchadores,
sin dibujar sólo cambia el actor `+0xac` en cada re-corrida (la hora del último dibujo: la primera corrida dibujó, la
otra no) y lo que el dibujo ya dejaba aparte (`+0xc8`, `+0x1b4`); dibujando, las re-corridas además tenían palabras del
actor de `+0x2c0` a `+0x2dc` y de `+0x580` a `+0x59c` distintas (el hilo de render escribiéndolas a su tiempo), sin
dibujar ninguna. Una grabación de 800 cuadros de esa pelea con B2 (`flash_scan.py`): ningún cuadro negro ni con basura
y ningún destello de un cuadro. Después, el SyncTest con B2 en cinco peleas de la Escalera con otro luchador del
jugador 1 cada una (1, 2, 3, 4 y 6 de la selección; el rival y el escenario, los de la Escalera; la CPU peleando contra
un jugador quieto; D3D12): 875-925 rollbacks de 6 cuadros en cada una, ~4500 en total, **0 cuadros distintos**.
**Falta** para encenderlo por defecto: el hunt entre dos juegos y peleas con X-Ray y fatality (el plan: "Prueba");
quedó para cuando la PC tenga memoria (la regresión 5 la cortó Claude Code por falta de memoria: el juego del
usuario, Discord, Chrome y los dos juegos de prueba).
