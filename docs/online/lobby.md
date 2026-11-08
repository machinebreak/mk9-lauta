# El online de punta a punta: lobby, salas, sesión y pantallas

Cómo se encuentran dos jugadores y llegan a pelear con rollback. El netcode en sí (lockstep en menús, rollback en la
pelea, el borde en reposo, el sonido afuera) está en `netplay-design.md`.

## Piezas

| Pieza | Archivo | Qué hace |
|---|---|---|
| Lobby | `app/src/online/lobby.h` | WebSocket (WinHTTP) al lobby compartido: nombre, jugadores en línea, salas abiertas, chat global, partida rápida, relay de mensajes entre los dos de una sala |
| Camino propio | `ice_link.h` (libjuice 1.7.4) | ICE entre las dos PCs: directo, por los NAT, o por el TURN del lobby; el apretón de manos va por el relay de la sala |
| Sala | `room.h` | Una sala de dos: crear (pública o privada), unirse (lista, código o partida rápida), chat de la sala, listos, arranque |
| Sesión | `session.h` | Al arrancar la sala, cada juego se vuelve a abrir ya unido, con un perfil nuevo |
| Pantallas online | `online_ui.h` (F1) | Nombre, lobby, salas, chats; en la sesión, la insignia del rival y el fin de la sesión |
| Opciones | `../../app/src/options_ui.h` (F2) | Presets gráficos y opciones en castellano; el retraso de entrada de las peleas |
| Perfil del P2 | `../../app/src/mk9_profile_prompt.cpp` | El jugador sin perfil de Xbox sigue sin guardar, sin la pregunta del juego |

## El lobby

El servidor es el que comparten los juegos web (`scripts/lobby_server.py` de ps2-web-recomp; en el VPS,
`wss://107-172-143-35.sslip.io/lobby?game=mk9pc`), con un lobby por juego (`game=mk9pc`). Mensajes JSON:

- cliente → servidor: `hello {v:2, sub, name}`, `name`, `chat`, `host {room, title, open, mode, status}`, `unhost`,
  `join {room, id}`, `sig {room, to: "host" | "guest" + id, data}`, `quick {mode}`, `sub`, `ping` (cada 25 s);
- servidor → cliente: `welcome`, `me`, `rooms`, `online`, `chat`, `sys`, `notice`, `del`, `error`, `match`, `sig`,
  `pong`; `GET /ice` da los servidores STUN/TURN.

El cliente se reconecta solo (con espera creciente) y vuelve a saludar con su nombre. El nombre "lauta" está
reservado al staff en el servidor (las pruebas usan Prueba1 / Prueba2).

**Las pruebas nunca usan el lobby real** (es compartido): `tools/online/room_test.ps1` y `pair_test.ps1 -Room` apuntan
a una copia local (`python lobby_server.py 8791 --host 127.0.0.1`), y `run_game.ps1` y `pair_test.ps1` sin sala pasan
`--mk9_lobby_url=off`. Un valor vacío en la línea de comandos **no** apaga el lobby (queda el de fábrica): para
apagarlo, `off`. Publicar o tocar el servidor del VPS necesita el permiso del dueño.

## La sala

Mensajes por el relay (`sig`, campo `room`): `hi {name, id}` (el invitado, cada segundo hasta que lo reciben;
10 s sin respuesta: la sala no responde), `welcome {name}` o `full`, `chat {text}`, `ready {on}`, `start` (el host),
`bye`, `ping` cada 3 s (12 s sin noticias: el otro se fue; en la sesión no cuenta, manda el camino propio). Códigos
de 6 caracteres sin I, O, 0 ni 1. Partida rápida: el lobby da la sala que más espera, o abre una nueva.

Cuando los dos están listos, el host manda `start`: el invitado se vuelve a abrir a los 300 ms y el host a los
1200 ms (que el invitado reciba el arranque antes).

## La sesión

`session::Launch` vuelve a abrir el mismo ejecutable con sus opciones (menos las de una sesión anterior y las de
prueba) más `--mk9_net_room`, `--mk9_net_role`, `--mk9_online_name` y `--user_data_root=online_session_host|guest`
(junto al ejecutable, borrado y creado de nuevo: los dos juegos arrancan del mismo estado y corren en lockstep desde
el primer cuadro). Los menús del juego llevan a los dos a VERSUS → ONE VS ONE → selección de luchador → pelea con
rollback (cualquiera de los dos puede manejar los menús; el que no los maneja se suma con START en la pantalla de 2
jugadores).

El jugador 2 no tiene perfil de Xbox (el SDK firma sólo al usuario 0): al manejar un menú, el juego le preguntaba
"¿continuar sin perfil?" (`XamShowMessageBoxUI` en una tarea del pool) y el menú quedaba con el dragón girando para
siempre (nadie contesta un cartel en una sesión, y cada máquina lo vería terminar en otro momento).
`mk9_profile_prompt.cpp` contesta "continuar sin guardar" en el hilo del juego, el mismo cuadro en las dos.

### El fin de la sesión

`netplay::EndReason`: el jugador se fue (`Leave`), el otro se fue (su `BYE`, que dice por qué, o el `bye` de la
sala), sin noticias del otro por `mk9_net_timeout_s` (20 s), las dos partidas se desincronizaron
(`mk9_net_end_on_desync`, encendido: terminan las dos) o no pudo arrancar. La pantalla lo dice y vuelve sola al lobby
a los 10 s (o con su botón): `session::Back` abre el juego de nuevo sin la sesión y con `--mk9_online_show=true`
(las pantallas online abiertas).

## Las pantallas (F1)

Sin nombre guardado, la ventana se abre sola: el nombre es obligatorio para jugar online (`online_name.txt` junto al
ejecutable; `mk9_online_name` lo pisa). Pestañas: **Salas** (partida rápida, crear sala pública o privada, unirse con
código, lista de salas abiertas con "Entrar"), **Sala** (código y "Copiar código", host y rival, listo, salir, chat de
la sala), **Chat global** (hora, nombre, insignia de staff, errores del servidor: lento, links, silenciado). En la
sesión: arriba a la derecha "vs Rival - N ms" (y "Esperando a Rival... N s" si el juego espera sus entradas), la
pestaña Sala con el ping y "Terminar la sesión y volver al lobby"; con la ventana cerrada, un aviso arriba con cada
mensaje nuevo de la sala.

El overlay se queda con el teclado mientras un campo de texto lo tiene (no llega al juego) y dibuja con Segoe UI
(acentos y eñes).

## Pruebas

```powershell
# el lobby local (una vez, en otra consola)
python lobby_server.py 8791 --host 127.0.0.1
# el flujo entero: el host abre sala pública, el invitado usa partida rápida, los dos se vuelven a abrir y pelean
powershell -File tools\online\room_test.ps1 -Seconds 150
# con lag emulado, el host se va a los 118 s, y el invitado saca capturas con el overlay
powershell -File tools\online\room_test.ps1 -Seconds 150 -Extra "--mk9_net_lag_ms=40 --mk9_net_jitter_ms=10" `
    -HostExtra "--mk9_online_test_leave_s=118" -GuestExtra "--renut_capture_overlay=true --mk9_snapshot_s=100,122"
# una sesión de sala sin el relanzamiento, con perfiles nuevos
powershell -File tools\online\pair_test.ps1 -Room PRUEBA -Profiles fresh1 -HostScript out\f3\host_fight.txt `
    -GuestScript out\f3\guest_fight.txt
```

Opciones de prueba: `mk9_online_test=host|quick` (crea sala pública o busca partida, y se pone listo),
`mk9_online_test_leave_s` (se va de la sesión), `mk9_options_test_preset` (elige un preset al arrancar: escribe
mk9.toml), `renut_capture_overlay` (las capturas de `mk9_snapshot_s` con el overlay encima).

Resultado (2026-11-08, todo en esta PC, lobby local): sala → relanzamiento → pelea con rollback en las dos; con 40 ms
de lag + 10 ms de jitter, 178 y 54 rollbacks (hasta 4 cuadros), sin desync, 102 y 12 sonidos que antes se
duplicaban sonando una sola vez; el host se va a mitad de la pelea y el invitado ve "Prueba1 se fue de la sesión" y
vuelve al lobby.

## Falta

- Probar contra el lobby del VPS (con permiso): que el servidor tenga el juego `mk9pc` y su TURN para los NAT
  difíciles.
- Pruebas entre dos PCs en redes distintas (NAT reales, pérdida, jitter de verdad).
- El canal de chat de la sala durante la sesión depende del lobby: si se cae, la pelea sigue (va por el camino
  propio) pero el chat no.
