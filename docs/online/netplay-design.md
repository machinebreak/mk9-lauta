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
2. **Las sesiones grabadas** (`mk9_net_record`, activado): cada sesión escribe sus cuadros finales en
   `logs/session_<fecha>_p1|p2.mk9net` (las entradas de los dos y el hash de sincronía de cada cuadro; quedan las 10
   últimas). `tools/online/session_report.py <p1> <p2>` dice si las entradas coinciden y en qué cuadro se separaron
   los hashes, con los botones de los cuadros de alrededor; con un archivo, sus épocas. Al detectar un desync el log
   dice dónde está el archivo, para que los jugadores lo manden.
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
