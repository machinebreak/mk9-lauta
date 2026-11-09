<p align="center"><img src="app/icon.jpg" width="128" alt="mk9-lauta"></p>

# mk9-lauta

**Mortal Kombat Komplete Edition nativo en PC, con online con rollback.**

*English: a native Windows port of Mortal Kombat Komplete Edition (Xbox 360, title 575207FD), made from the
[mk9-nx](https://github.com/Thorhax/mk9-nx) static recompilation, with online play (rollback netcode, a lobby with
rooms and chat), a first-start installer that takes your own game disc image, and settings for low-end PCs. No game
files are included: you need your own copy of the game.*

mk9-lauta es el juego de Xbox 360 recompilado a un ejecutable de Windows: no es un emulador. Está hecho sobre
[mk9-nx](https://github.com/Thorhax/mk9-nx) de Thorhax (la recompilación estática con el renderer Renut) y le suma el
online, el instalador, las opciones y los controles de PC.

**Este repositorio no tiene ningún archivo del juego.** Hace falta una copia propia del juego original de Xbox 360,
como imagen `.iso` del disco.

## Qué trae

- **Online con rollback** (F1): lobby con nombre de usuario, partida rápida, salas públicas y privadas con código,
  chat de la sala y chat global. Las peleas usan rollback (si la conexión tiene un salto, el juego se corrige solo
  sin frenar) y los menús van sincronizados. Conexión directa entre las dos PCs, y por un servidor de relevo cuando
  el router no la deja.
- **Instalador**: la primera vez elegís la ISO del juego (o la carpeta donde ya está extraído) y se verifica y copia
  solo.
- **Opciones** (F2): calidad gráfica (Muy baja, Baja, Consola, Alta, Ultra), resolución de 480p a 4K, sombras,
  oclusión ambiental, filtrado de texturas, antialiasing, pantalla completa, contador de FPS, DirectX 12 o Vulkan,
  idioma del juego (español con el Modo Historia doblado, inglés, portugués, francés, alemán, italiano).
- **PCs bajas**: la primera vez el juego elige la calidad según tu placa de video y tu memoria. En "Muy baja", si la
  PC no llega a 60 cuadros, dibuja uno de cada dos y el juego sigue a su velocidad. Con 4 a 6 GB de RAM se activa solo
  el ahorro de memoria.
- **Controles**: mandos de Xbox, PlayStation y Switch Pro, con vibración, y el teclado listo para pelear, con cada
  tecla cambiable.

## Requisitos

- Windows 10 u 11 de 64 bits.
- Una placa de video compatible con DirectX 12 o Vulkan (también sirven las integradas).
- 4 GB de RAM o más.
- ~8 GB libres en el disco.
- Tu copia de **Mortal Kombat Komplete Edition** de Xbox 360 como imagen `.iso` del disco (la versión europea: inglés,
  francés, alemán, español, italiano, portugués). No sirven las imágenes comprimidas (CCI, GOD, ZAR).

## Instalación

1. Bajá el `.zip` de la última versión en [Releases](../../releases) y descomprimilo donde quieras, en un disco con
   ~8 GB libres (no adentro de "Archivos de programa").
2. Abrí `mk9.exe`. La primera vez se abre el instalador: elegí tu `.iso` (o la carpeta del juego, la que tiene
   `default.xex`). Se revisa que sea la versión correcta y se copian los archivos del juego a `game_root`. Es una sola
   vez: después arranca directo.
   - Si Windows muestra "Windows protegió tu PC" (el programa no tiene firma digital): "Más información" y "Ejecutar
     de todas formas".
   - La primera vez que entrás al online, Windows pregunta si dejás que el juego use la red: "Permitir" (redes
     privadas y públicas).
3. La primera vez tarda un poco más en arrancar mientras prepara los gráficos de tu placa de video.

## Jugar online

**F1** abre el online. La primera vez te pide un nombre. Para pelear con un amigo, uno toca **Crear sala privada** y
le pasa el código de 6 letras; el otro lo pone en **Unirse con código**. Cuando los dos tocan **Estoy listo**, el juego
se vuelve a abrir solo, ya conectado. En el menú del juego, VERSUS: uno elige el modo y el otro se suma con START.

Todo el detalle (salas, controles, opciones y qué hacer si algo falla): **[guía para jugadores](docs/online/guia-jugadores.md)**.

## Si algo falla

- La carpeta `logs` (junto a `mk9.exe`) tiene lo que pasó: el último `mk9_*.log` y, si el juego se cerró de golpe, el
  `crash_*.dmp` de la misma hora. Abrí un [issue](../../issues) con eso.
- El juego anda lento: F2 > Gráficos > Calidad "Baja" o "Muy baja", y "Mostrar FPS" para ver cómo va.
- Se ve mal o no arranca el video: F2 > Pantalla > API gráfica: probá con la otra (Vulkan o DirectX 12).

## Compilar

[docs/building.md](docs/building.md). El diseño del online: [docs/online](docs/online/README.md).

## Créditos

mk9-lauta existe gracias a [mk9-nx](https://github.com/Thorhax/mk9-nx) de **Thorhax** y a todo lo que mk9-nx usa:
[usf4-nx](https://github.com/Thorhax/usf4-nx), [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx) de
ChanseyIsTheBest, [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx) de NebadasSwifty,
[nfsmw-nx](https://github.com/StevensND/nfsmw-nx) de StevensND, el [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
de Tom Clay (con partes de [Xenia](https://xenia.jp)), el renderer [reNut](https://github.com/masterspike52/reNut) de
masterspike52 (con su permiso, como lo usan banjo-nx, sa2-nx y usf4-nx), [plume](https://github.com/renderbag/plume),
[XenosRecomp](https://github.com/hedge-dev/XenosRecomp), [libjuice](https://github.com/paullouisageneau/libjuice),
[SDL](https://www.libsdl.org), [FFmpeg](https://ffmpeg.org), [Dear ImGui](https://github.com/ocornut/imgui) y más:
la lista completa con sus licencias está en [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Licencia

GPL-3.0, ver [LICENSE](LICENSE). Los componentes de terceros tienen sus propias licencias
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)).

Proyecto de fans, gratuito y sin fines de lucro. Mortal Kombat es una marca de Warner Bros. Entertainment; este
proyecto no está afiliado ni respaldado por Warner Bros. Games, NetherRealm Studios ni Microsoft, y no incluye ningún
archivo del juego.
