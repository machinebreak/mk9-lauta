# Prueba entre dos PCs reales (tarea A7 del plan 0.4)

Lo único que las pruebas en una sola PC no pueden ver: dos máquinas distintas (otra CPU, ideal una Intel contra una
AMD; otra placa de video; otro disco), una red de verdad entre las dos (con su jitter y sus pérdidas) y el relay
(TURN) cuando no hay camino directo. Una tarde de ~40 minutos con un tester alcanza.

## Antes

- Los dos con **la misma versión** de mk9-lauta (F2 > Acerca de). Si no, el juego avisa al conectar.
- Cada uno con su nombre en F1. Mandos o teclado, como cada uno juegue.
- Anotar de cada PC: procesador, placa de video, RAM y si es notebook (para cruzar con lo que salga).

## Las partidas

1. **Tres peleas seguidas con revancha** (REMATCH), en una sala privada: uno la crea (F1 > Crear sala privada) y le
   pasa el código al otro. Una de las tres, con alguien eligiendo otro luchador (SELECT A NEW FIGHTER).
2. **Una pelea por el relay**: los dos cierran el juego y lo abren con un acceso directo que agregue al final del
   destino `--mk9_ice_relay_only=true` (clic derecho en `mk9.exe` > Crear acceso directo > Propiedades > Destino:
   `"...\mk9.exe" --mk9_ice_relay_only=true`). La sala y la pelea, igual que antes. El cartel de la sesión dice si va
   por relay.
3. **Una partida rápida** (F1 > Partida rápida) los dos a la vez, para ver que se encuentren solos.
4. Si se puede, **un espectador**: una tercera PC con la misma versión aprieta Mirar en la sala que juega, a mitad de
   la primera pelea (tiene que ponerse al día y seguir la pelea igual que los jugadores).

Durante las partidas, anotar lo raro con la hora: un tirón, un golpe que uno vio y el otro no, el sonido distinto,
un corte.

## Después: qué mandar

De **cada** PC, de la carpeta `logs` que está al lado de `mk9.exe`:

- los `session_*.mk9net` de ese día (las entradas de cada partida);
- los `mk9_*.log` de ese día (el registro del juego; los que se modificaron más tarde);
- si aparece alguna carpeta `desync_*` (el juego la deja si las dos partidas dejaron de coincidir; la ventana del final
  de la sesión tiene un botón que la abre), esa carpeta entera. También las `desync_*_fighters`: las deja el hash de
  los luchadores cuando difiere sin que el juego lo note (un diagnóstico, no le avisa a nadie), y dicen en qué cuadro
  y en qué campo.

Comprimir todo en un zip por PC y mandarlo.

## Análisis (para quien lo reciba)

- `python tools/online/session_report.py <session_p1.mk9net> <session_p2.mk9net>`: dónde se separaron las dos
  partidas, si se separaron, con las entradas de los dos.
- `python tools/online/desync_report.py <desync_..._p1> <desync_..._p2>`: el primer cuadro y el primer campo de los
  luchadores que difirió (vida, posición, …), o si la diferencia está en los sorteos del juego.
- En los `mk9_*.log`: las líneas `[net]` cada 10 s (ida y vuelta, retraso, rollbacks y su profundidad, la fase de las
  dos grillas: "phase X ms ahead"), `[fps]` (los cuadros por segundo y sus percentiles), `[frame]` (dónde se va el
  tiempo del hilo del juego), `[time]` (el reloj de cuadros), cualquier `DESYNC` y cualquier "the fighters' state
  differs" (el diagnóstico: el reporte `_fighters` de cada PC va a `desync_report.py` igual que los otros).

**Listo cuando**: una sesión real sin desync, o cada diferencia explicada y arreglada.
