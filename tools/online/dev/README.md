# Herramientas de prueba del online (desarrollo)

Scripts de las pruebas de espectadores, sonido de la pelea y desyncs de menús (2026-11-10). Corren desde cualquier
carpeta: toman la raíz del repo de su propia ubicación. Las que usan salas (`-Room`, `WATCH1`) necesitan el **lobby
local** corriendo (la copia de `scripts/lobby_server.py` de ps2-web-recomp, que no está en este repo):

```powershell
python <ruta>\lobby_server.py 8791 --host 127.0.0.1
```

Nunca apuntan al lobby real.

| Script | Qué hace |
|---|---|
| `fight_watch.ps1 [-Seconds 260] [-WatchDelay 3] [-Lag 40] [-Jitter 10] [-More "--cvar=x ..."]` | Los dos jugadores pelean (guiones `out\f3\host_long.txt` / `guest_long.txt`) en la sala WATCH1 con latencia emulada, y un espectador mira desde el principio (o entra a los `-WatchDelay` s). `-More` va a los tres juegos |
| `two_watchers.ps1 [-Seconds 230] [-LateDelay 95] [-More ...]` | Igual, con dos espectadores: `live` desde el principio y `late` que entra tarde y adelanta. Sus `out\f3\<nombre>_draws.txt` / `_natives.txt` comparados dicen qué cambia al adelantar (ninguno re-corre cuadros) |
| `watcher.ps1 -Delay s -Seconds s [-Name watch] [-Extra ...] [-Snaps "56,98"]` | Un espectador de la sala WATCH1 (log `logs\<Name>.log`, chequeos, sorteos, nativas y sonido en `out\f3\<Name>_*.txt`). Para ver cómo se pone al día: `-Extra "--mk9_watch_catch_up_mode=paced"` (oculto al ritmo de los jugadores) o `drawn` (dibujado sin ritmo, con `--renut_vsync=false`) |
| `ui_watch.ps1 [-Seconds 90] [-Delay 12]` | La sala pública que juega queda en la lista y un tercer juego aprieta su botón Mirar (`mk9_online_test=watch`) |
| `sound_fight.ps1 [-Seconds 200] [-Lag 40] [-Jitter 10] [-More ...]` | Una pelea por UDP directo con `mk9_sound_log` en los dos (`out\f3\host_sound.txt(.frames)`) |
| `intro_hunt.ps1 [-Runs 8] [-Seconds 90] [-Extra ...] [-KeepGoing]` | Sesiones cortas hasta los primeros segundos de la pelea con los luchadores volcados en cada cuadro (al comienzo del borde y en el momento del hash). Sin `-KeepGoing` para en la primera cuyo hash de luchadores difiera; con él guarda los volcados de cada corrida para `fighter_diff.py` |
| `replay_hunt.ps1 [-Runs 6]` | Repite la sesión que se separó el 2026-11-09 (`out\f3\desync_menu_20261109`) con los botones de cada jugador desde su grabación (`mk9_net_input_replay` + `mk9_seed`) hasta que las dos máquinas se separen |
| `cmdlines.ps1 [-Seconds 90]` | Anota la línea de comandos de cada `mk9.exe` que aparece (los juegos relanzados por una sala) |
| `cmp_checks.py a b [contexto]` | La primera línea en que difieren dos `mk9_check_log` |
| `fighter_diff.py host.bin guest.bin [host2.bin guest2.bin ...] [--out rangos.txt\|fight_hash_ranges.inc] [--constants]` | Los luchadores de dos máquinas (`mk9_fighter_dump=<archivo>`, cada `mk9_fighter_dump_every` cuadros) palabra por palabra: qué tramos fueron siempre iguales y cambiaron (lo que entra al hash de la pelea). Con varias sesiones, una palabra tiene que ser igual en todas. Con `.inc` escribe la tabla que compila `fight_state.cpp` |
| `clock_audit.py host.txt guest.txt [--show 10]` | Las lecturas del reloj del hilo del juego de dos máquinas (`mk9_clock_audit`, `virtual_time.h`), por cuadro y por llamador: cuántas hace la lógica fuera de las esperas a otro hilo, cuántas dentro, y si algún cuadro pasó la válvula |
| `cmp_sound.py host.frames guest.frames` | Las entregas de sonido de la pelea y las palabras de los avisos, comparadas entre las dos máquinas |
| `handoff_report.py host.frames guest.frames [--depth 8]` | Las entregas de sonido que hizo una sola máquina, por cadena de llamadas del juego (con `mk9_sound_log`) |
| `rec_frames.py grabación.mk9net época desde hasta` | Los cuadros de una grabación: busy, ready, aviso de sonido, botones y hash de cada jugador |
| `rooms_probe.py [segundos]` | Las listas de salas que manda el lobby local |
| `local_chatters.py [segundos]` | Llena el lobby local con chat y salas de jugadores de varios países (para capturas) |
| `discord_check.py` | Saludo al pipe de Discord con el Application ID de mk9-lauta, sin poner actividad |

Las herramientas estables de análisis siguen en `tools\online\` (`session_report.py`, `rand_report.py`,
`stacks_report.py`, `pair_test.ps1`, `hunt.ps1`, ...).
