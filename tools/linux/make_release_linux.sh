#!/usr/bin/env bash
# mk9-lauta (2026-11-13): the Linux release folder and its .tar.gz, as tools/release/make_release.ps1 makes Windows':
# the game's executable and libraries (out/linux), what it needs beside it, the players' guide and the licences. No
# game files: the first start's installer makes game_root from the player's own disc image (app/src/mk9_installer.h).
#
#   tools/linux/make_release_linux.sh [--tar]          (in WSL or on Linux; OUT=out/release/mk9-lauta-linux)
#
# --tar writes mk9-lauta-v<version>-linux-x64.tar.gz beside the folder, the files at its root as in Windows' zip (the
# in-game updater looks for that name and lays the files out from the root). Its signature is made on the PC that has
# the release key: python tools/release/sign_release.py out/release/mk9-lauta-v<version>-linux-x64.tar.gz
#
# What goes in:
#   mk9, librexruntimerd.so, librexgpu-xenosrd.so   the build (out/linux); mk9 without its debug information, which
#                                                   stays beside the folder (mk9-v<version>.debug: crash reports'
#                                                   addresses, llvm-symbolizer --obj=mk9.debug)
#   gamecontrollerdb.txt, mk9.toml, pipeline_list.bin  as in Windows' (the pipeline list is the same for Vulkan)
#   mk9-lauta.png, crear-acceso-directo.sh          the game in the desktop's menu: an entry with its icon
#   LEEME.txt, AVISO.txt, licencias/                the guide, the notice, every licence
# Nothing is published or uploaded: the folder (and the .tar.gz with --tar) stays on this PC.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=${BUILD:-$REPO/out/linux}
OUT=${OUT:-$REPO/out/release/mk9-lauta-linux}
TAR=0
[ "${1:-}" = "--tar" ] && TAR=1
. "$REPO/tools/linux/clang20_env.sh"
VERSION=$(sed -n 's/^set(MK9_LAUTA_VERSION "\([^"]*\)").*/\1/p' "$REPO/app/CMakeLists.txt" | head -1)
[ -n "$VERSION" ] || { echo "MK9_LAUTA_VERSION not found in app/CMakeLists.txt" >&2; exit 1; }
OBJCOPY=$(command -v llvm-objcopy || command -v objcopy)
SDK_LIB=$REPO/sdk/out/linux-amd64

rm -rf "$OUT"
mkdir -p "$OUT/licencias"

# --- the build ---
[ -x "$BUILD/mk9" ] || { echo "missing from the build: $BUILD/mk9" >&2; exit 1; }
cp "$BUILD/mk9" "$OUT/mk9"
"$OBJCOPY" --only-keep-debug "$OUT/mk9" "$(dirname "$OUT")/mk9-v$VERSION.debug"
"$OBJCOPY" --strip-debug --add-gnu-debuglink="$(dirname "$OUT")/mk9-v$VERSION.debug" "$OUT/mk9"
for lib in librexruntimerd.so librexgpu-xenosrd.so; do
  src=$BUILD/$lib
  [ -f "$src" ] || src=$SDK_LIB/$lib
  [ -f "$src" ] || { [ "$lib" = librexruntimerd.so ] && { echo "missing: $lib" >&2; exit 1; }; continue; }
  cp -L "$src" "$OUT/$lib"
  "$OBJCOPY" --strip-debug "$OUT/$lib"
done
for f in gamecontrollerdb.txt pipeline_list.bin; do
  for dir in "$BUILD" "$REPO/out/pc"; do
    if [ -f "$dir/$f" ]; then cp "$dir/$f" "$OUT/"; break; fi
  done
done
[ -f "$OUT/gamecontrollerdb.txt" ] || { echo "missing: gamecontrollerdb.txt" >&2; exit 1; }
[ -f "$OUT/pipeline_list.bin" ] || echo "warning: no pipeline_list.bin: the first fights build their pipelines as they go"
cp "$REPO/app/mk9.toml" "$OUT/"
chmod 755 "$OUT/mk9"

# --- the desktop's menu: the icon (the port's own, app/icon.jpg) and an entry made where the folder is ---
python3 - "$REPO/app/icon.jpg" "$OUT/mk9-lauta.png" <<'PY' 2>/dev/null || cp "$REPO/app/icon.jpg" "$OUT/mk9-lauta.jpg"
import sys
from PIL import Image
Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])
PY
cat > "$OUT/crear-acceso-directo.sh" <<'SH'
#!/bin/sh
# Pone MK9 by Lauta en el menú de aplicaciones de tu escritorio (un acceso directo a esta carpeta, con su ícono).
# Si movés la carpeta, volvé a ejecutarlo.
dir=$(cd "$(dirname "$0")" && pwd)
icon="$dir/mk9-lauta.png"
[ -f "$icon" ] || icon="$dir/mk9-lauta.jpg"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
cat > "$apps/mk9-lauta.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=MK9 by Lauta
Comment=Mortal Kombat Komplete Edition con online y rollback
Exec="$dir/mk9"
Path=$dir
Icon=$icon
Terminal=false
Categories=Game;
EOF
chmod 644 "$apps/mk9-lauta.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1
echo "Listo: MK9 by Lauta está en el menú de aplicaciones."
SH
chmod 755 "$OUT/crear-acceso-directo.sh"

# --- the guide, the notice ---
{
cat <<EOF
MK9 by Lauta $VERSION para Linux: Mortal Kombat Komplete Edition para PC, con online y rollback
Hecho por Lauta. Página oficial (lo único oficial, gratis): https://github.com/machinebreak/mk9-lauta
Comunidad, rivales y ayuda: https://discord.gg/7hbqkDmKHA

INSTALACIÓN
===========

1. Descomprimí este archivo donde quieras, en un disco con ~8 GB libres (por ejemplo en tu carpeta personal).
2. Abrí mk9 (doble clic, o en una terminal dentro de la carpeta: ./mk9). Para tenerlo en el menú de aplicaciones,
   ejecutá crear-acceso-directo.sh una vez. La primera vez se abre el instalador: elegí la imagen .iso de tu copia de
   Mortal Kombat Komplete Edition de Xbox 360 (la versión europea: En, Fr, De, Es, It, Pt), o la carpeta donde ya la
   tenés extraída (la que tiene default.xex); también podés arrastrarla a la ventana. Se revisa que sea la versión
   correcta y, desde la ISO, se copian los ~7 GB que usa el juego a la carpeta game_root. Una sola vez: después el
   juego arranca directo.
3. La primera vez tarda un poco más en arrancar: prepara los gráficos de tu placa de video.

En Steam Deck: en el modo escritorio, Steam > Agregar un juego que no es de Steam > el archivo mk9 de esta carpeta.

Requisitos: Linux de 64 bits reciente (glibc 2.39 o más nueva: Ubuntu 24.04, Linux Mint 22, Fedora 40, Debian 13, Arch,
SteamOS 3.6 o más nuevos), una placa de video con Vulkan 1.2 y su driver (Mesa 23 o más nuevo para AMD e Intel, el
driver propietario 535 o más nuevo para NVIDIA), 4 GB de RAM o más, y una sesión gráfica (X11, o Wayland con XWayland).
El online usa libcurl, que viene en todas las distribuciones. La primera vez el juego elige la calidad según tu PC; se
cambia en F2 > Gráficos (para PCs con poca placa de video: "Baja" o "Muy baja").

Online entre Linux y Windows: se juega igual, contra cualquiera, con la misma versión del juego en las dos PCs.

Si algo falla, la carpeta logs tiene lo que pasó: mandá en el Discord el último archivo mk9_*.log (y, si el juego se
cerró de golpe, el crash_*.txt de la misma hora).


EOF
cat "$REPO/docs/online/guia-jugadores.md"
} > "$OUT/LEEME.txt"

cat > "$OUT/AVISO.txt" <<'EOF'
AVISO

Este programa es un trabajo de fans, gratuito y sin fines de lucro. No está afiliado, asociado, autorizado ni
respaldado por Warner Bros. Games, NetherRealm Studios, Microsoft ni ningún otro dueño de los derechos del juego
original. No incluye ningún archivo del juego: hace falta una copia propia y legítima del juego original de Xbox 360.
Todas las marcas pertenecen a sus dueños.

El código de este programa se distribuye bajo la licencia GNU GPL versión 3 (licencias/mk9-lauta GPL-3.0.txt); las
bibliotecas de terceros, bajo las suyas (carpeta licencias, y los créditos en licencias/THIRD_PARTY_NOTICES.md).
El código fuente: https://github.com/machinebreak/mk9-lauta
EOF

# --- licences: this project's, the SDK's, every third-party component's, the controller database's ---
lic=$OUT/licencias
cp "$REPO/LICENSE" "$lic/mk9-lauta GPL-3.0.txt"
cp "$REPO/THIRD_PARTY_NOTICES.md" "$lic/"
cp "$REPO/sdk/LICENSE" "$lic/ReXGlue SDK.txt"
cp "$REPO/app/gamecontrollerdb.LICENSE.txt" "$lic/SDL_GameControllerDB.txt"
for dir in "$REPO"/app/thirdparty/*/ "$REPO"/sdk/thirdparty/*/; do
  name=$(basename "$dir")
  for f in "$dir"*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    case "$base" in
      LICENSE*|LICENCE*|COPYING*|COPYRIGHT*|NOTICE*|CLDR-LICENSE*) cp "$f" "$lic/$name - ${base%.*}.txt" ;;
    esac
  done
done

files=$(find "$OUT" -type f | wc -l)
echo "$OUT: $files files, $(du -sm "$OUT" | cut -f1) MB"
if [ "$TAR" = 1 ]; then
  pkg=$(dirname "$OUT")/mk9-lauta-v$VERSION-linux-x64.tar.gz
  rm -f "$pkg"
  # the files at the root, in a fixed order with fixed owners (the same archive from the same files)
  tar --sort=name --owner=0 --group=0 --numeric-owner -C "$OUT" -czf "$pkg" .
  echo "$pkg: $(du -m "$pkg" | cut -f1) MB"
  echo "sign it on the PC with the release key: python tools/release/sign_release.py ${pkg#$REPO/}"
fi
