# mk9-lauta

[![Release](https://img.shields.io/github/v/release/machinebreak/mk9-lauta?label=release)](https://github.com/machinebreak/mk9-lauta/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/machinebreak/mk9-lauta/total)](https://github.com/machinebreak/mk9-lauta/releases)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)

A native PC port of **Mortal Kombat Komplete Edition** (Xbox 360, PAL release, title 575207FD) with online play,
built on the static recompilation in **[mk9-nx](https://github.com/Thorhax/mk9-nx)** by Thorhax. It is the game's
own code recompiled for PC, with a new renderer, sound and input layer underneath; it is not an emulator.

This repository contains **no game data**. You need your own disc image of that release: the first start reads the
game's files from it, and they stay on your computer.

> **Official source.** This project lives only at
> [github.com/machinebreak/mk9-lauta](https://github.com/machinebreak/mk9-lauta), and it never ships the game:
> you need your own disc. Our releases are free and have no ads, accounts or payments.

**En español:** la guía para jugadores (instalación, online, controles y opciones) está en
[docs/online/guia-jugadores.md](docs/online/guia-jugadores.md) y en el `LEEME.txt` de cada release.

## What works

- The whole game: Story, Ladder and Tag Ladder, Versus with two players on one PC, the Challenge Tower, Training
  and the Krypt, with the Komplete Edition's downloadable fighters.
- A GPU renderer (Renut, from masterspike52's reNut) on Direct3D 12, or on Vulkan (F2 > Pantalla e idioma > API
  gráfica; Vulkan is also tried by itself when Direct3D 12 cannot start). Internal resolution 480p, 720p (the
  console's), 1080p, 1440p or 4K; shadows, ambient occlusion, texture filtering, effect precision and MSAA can each
  be set. **Resolución dinámica** (dynamic resolution) lowers the internal resolution a step while the PC cannot
  keep 60 frames a second (an X-Ray, many effects) and raises it again when it can.
- Low-end PCs: the first start picks a quality preset from the graphics card and the memory (Muy baja, Baja,
  Consola, Alta, Ultra; F2 > Gráficos changes it). **Cuadros dibujados** (drawn frames) keeps the game at 60 frames
  a second and draws one picture in two (half the graphics card's work), always or only while the PC cannot keep
  up; a fight goes at the same speed either way. **Ahorro de memoria** (memory saving) turns itself on with 6 GB of
  RAM or less. F2 > Pantalla e idioma > Mostrar FPS shows the frame rate.
- Keyboard and controllers (Xbox, PlayStation and Switch Pro, through SDL), with vibration. The keyboard comes set
  for fighting (WASD or arrows, U I J K, O, L, Ñ, P) and every key can be changed in F2 > Controles; the keyboard
  can play player 1 or player 2, for a versus with a controller on the same PC. The game's button prompts follow
  the first controller connected (PlayStation, Switch or Xbox buttons) or show the keyboard's keys when there is
  none; F2 > Controles fixes one style.
- The disc's six languages: English, French, German, Spanish (with the dubbed Story mode), Italian and Portuguese.
- A first-start installer: choose your disc image (or the folder you extracted it to); it checks that it is the
  right release and copies the ~7 GB the game uses next to the program. At every start the game checks its files
  and names a missing or damaged one.
- In the game, **F1** opens the online screens and **F2** the settings (Gráficos, Controles, Pantalla e idioma,
  Online, Acerca de). With a controller: **BACK + START** opens the settings and **BACK + Y** the online screens,
  **LB / RB** change tabs and **B** closes. **F11** or **Alt+Enter** switch between full screen and a window;
  F2 > Pantalla e idioma also chooses the monitor, the window's size and the size of the F1/F2 menus, which fit
  small screens such as 1366x768. Closing the window asks first. After a crash, `logs` keeps the log and a
  `crash_*.dmp` of it.
- **Updates inside the game:** at start the game asks GitHub whether there is a newer release. If there is, a
  notice offers to download it; the download is checked against the project's signature and installed when the
  game restarts, keeping your saves, settings and game files. F2 > Acerca de checks by hand or turns it off.
  (Version 0.1.0 had no updater: from it, download 0.2.0 once by hand.)

- **Online play, experimental:** the main menu's **ONLINE (F1)**. Choose a name, then **Partida rápida** (quick
  match: the room that has waited longest, or a new one of yours), **Crear sala pública**, **Crear sala privada** (a
  six-character code to pass to a friend) or **Unirse con código**. A room has its own chat, and the lobby a global
  one. When both players press **Estoy listo** (ready), the game starts again already connected. The game's own
  menus are shared: either player chooses VERSUS (ONE VS ONE or TWO VS TWO), the other joins with START, and each
  picks a fighter. Fights run with rollback: your own moves come out at once (with one frame of input delay unless
  the host chooses 0 or 2, raised by itself with a long ping, all fight long, so rollbacks stay about five frames
  deep) and the game
  corrects itself when the other player's buttons arrive. Menus and loads run
  in lockstep, with their delay set from the ping. There is no pause in an online fight. The opponent's name and
  ping show at the top right; when the match ends, the loser chooses REMATCH, another fighter or the menu.

Online play is new. It has been tested with two games on one PC over emulated networks and through the real
lobby: X-Ray-heavy fights with 3 % packet loss and heavy jitter, and 21-minute sessions of 11 fights with ~90 ms
and with ~200 ms of ping (jitter, 1 % loss), where the two games stayed the same from the first frame to the last
(75,000-77,000 frames compared). The 0.1.0 testers' logs (Argentina-Venezuela, 140-220 ms, partly over the TURN
relay) showed sessions frozen at their start, slow menus, slow fight intros, crashes and desyncs in fights: all
traced and fixed in 0.2.0 (docs/online/netplay-design.md). The 0.2.0 testers' reports (the screen flickering in
online fights with "green lights", a black scene or coloured triangles; sessions frozen at the logo; fights that got
worse as the ping rose) were reproduced on one PC, by recording what each game showed, and fixed in 0.2.1. Both
players need the same release: since 0.2.1 the game says so when they do not. Each online session saves both players' inputs in
`logs/session_*.mk9net` (the newest ten): if two games ever part, the two players' files say where
(`tools/online/session_report.py`). [docs/online/netplay-design.md](docs/online/netplay-design.md) is the working
log of how it is built and what was checked.

Rooms, quick matches and chat go through a lobby server (a WebSocket service). The game sends it the name you
choose and, in a room, the messages the two games exchange to find each other. The games then send to each other
directly. Where the routers do not allow that, the match goes through a relay (Cloudflare's TURN service), which
passes the match's packets on. `mk9_lobby_url` in `mk9.toml` points the game at another lobby.

## Requirements

- Windows 10 or 11, 64-bit.
- A graphics card with Direct3D 12 or Vulkan (integrated graphics work too; see the presets above).
- 4 GB of RAM or more.
- About 8 GB of disk space, and your disc image as a plain `.iso` file (the PAL release: English, French, German,
  Spanish, Italian, Portuguese). Compressed images (CCI, GOD, ZAR) are not read.

## Installing a release

Unpack the release where you like (not inside "Program Files"), start `mk9.exe` and choose your disc image, or the
folder with the extracted disc (the one with `default.xex`). The installer checks the release, copies the game's
files to `game_root` next to the program and starts the game; later starts go straight in.

Windows SmartScreen may say "Windows protected your PC", as the program has no signature: "More info", "Run
anyway". The first time you go online, Windows asks whether the game may use the network: allow it on private and
public networks. The first start takes a little longer while the graphics are prepared for your card.

## Building from source

[docs/building.md](docs/building.md): Visual Studio 2022 Build Tools, LLVM clang 20, Python 3 and Git, about 30 GB
free. The build extracts your disc, builds the code generator, translates the game's code from your `default.xex`
(into `app/generated`, which is not in this repository) and builds the program.
`tools/release/make_release.ps1 -Zip` packs a release (the program, Visual C++'s runtime, the guide and the
licences; no game data).

## Layout

| Path | Contents |
|---|---|
| `app/src/` | The PC side: the settings and controls windows, the installer, hardware detection, texts, frame pacing |
| `app/src/online/` | Online play: lobby, rooms, ICE (libjuice), sessions, the netcode (lockstep and rollback), the memory timeline, sound synchronization |
| `app/src/renut_engine/` | Renut's renderer, retargeted to MK9's Direct3D (Direct3D 12 and Vulkan through plume) |
| `app/thirdparty/` | libjuice, plume, Dear ImGui, the shader translator and its tools |
| `sdk/` | The ReXGlue SDK: code generator, Xbox 360 kernel and runtime, audio, input, files (with the port's changes) |
| `shaders/` | XenosRecomp, the Xbox 360 shader translator |
| `tools/` | Build, test (`tools/online`) and release (`tools/release`) scripts |
| `docs/` | Notes on the game and the port; `docs/online/` on the netcode, and the players' guide (Spanish) |

This project was made with the help of AI.

## Credits

- [mk9-nx](https://github.com/Thorhax/mk9-nx) by **Thorhax**: the static recompilation of the game this port is
  built on (the first commit here is mk9-nx 1.0.4, unchanged). mk9-nx comes from
  [usf4-nx](https://github.com/Thorhax/usf4-nx) by Thorhax, which comes from
  [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx) by ChanseyIsTheBest,
  [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) by NebadasSwifty,
  [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) by StevensND and
  [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) by madelrandel-blip.
- The [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) by Tom Clay, with parts of [Xenia](https://xenia.jp);
  [reNut](https://github.com/masterspike52/reNut) by masterspike52, used with its author's permission;
  [plume](https://github.com/renderbag/plume), [XenosRecomp](https://github.com/hedge-dev/XenosRecomp),
  [libjuice](https://github.com/paullouisageneau/libjuice), [SDL](https://www.libsdl.org),
  [FFmpeg](https://ffmpeg.org) and [Dear ImGui](https://github.com/ocornut/imgui).

The full list, with every licence: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licence

The port's code in `app/`, `tools/` and `docs/` is under the [GNU General Public License, version 3](LICENSE)
(GPL-3.0); the SDK (`sdk/`) under BSD-3-Clause ([sdk/LICENSE](sdk/LICENSE)) and the shader translator (`shaders/`)
under MIT ([shaders/LICENSE.md](shaders/LICENSE.md)). Third-party components keep their own licences
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). Whoever passes the program on, changed or not, has to offer its
source under the same licence. The game's code, translated from the disc when the program is built, is not ours to
license: it belongs to the game's rights holders.

## Legal

This project is not affiliated with or endorsed by Warner Bros. Games, NetherRealm Studios, Microsoft or any other
rights holder of the game. It exists for preservation, study and interoperability, and distributes none of the
game's assets. Mortal Kombat is a trademark of Warner Bros. Entertainment; all rights to the game belong to their
owners. You need your own legally obtained copy of the game to use it.
