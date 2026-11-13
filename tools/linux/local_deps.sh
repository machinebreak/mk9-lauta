#!/usr/bin/env bash
# mk9-lauta (2026-11-13): the build's missing -dev packages without root: downloaded (apt-get download) and unpacked
# into a prefix of their own (default ~/mk9-deps), with their pkg-config files pointed there and their libraries' .so
# links pointed at the system's runtime libraries. For a machine where `sudo apt install` is not at hand.
#   tools/linux/local_deps.sh [pkg ...]       then: PKG_CONFIG_PATH=~/mk9-deps/pc CONF_EXTRA=... build_linux.sh conf
set -euo pipefail
PREFIX=${PREFIX:-$HOME/mk9-deps}
pkgs=("$@")
[ ${#pkgs[@]} -gt 0 ] || pkgs=(libx11-xcb-dev libcurl4-openssl-dev)
mkdir -p "$PREFIX/debs" "$PREFIX/root" "$PREFIX/pc"
cd "$PREFIX/debs"
for p in "${pkgs[@]}"; do
  apt-get download "$p" >/dev/null 2>&1 || { echo "could not download $p" >&2; exit 1; }
done
for deb in *.deb; do dpkg -x "$deb" "$PREFIX/root"; done
lib="$PREFIX/root/usr/lib/x86_64-linux-gnu"
# the -dev packages' .so links are relative (libX.so -> libX.so.1): point them at the system's runtime libraries
for link in "$lib"/*.so; do
  [ -L "$link" ] || continue
  target=$(readlink "$link")
  if [ ! -e "$lib/$target" ] && [ -e "/usr/lib/x86_64-linux-gnu/$target" ]; then
    ln -sf "/usr/lib/x86_64-linux-gnu/$target" "$link"
  fi
done
# their pkg-config files, with the prefix where they were unpacked
for pc in "$lib"/pkgconfig/*.pc; do
  # every path into the unpacked tree (some .pc files write /usr/include/... instead of ${prefix})
  sed -e "s|^prefix=.*|prefix=$PREFIX/root/usr|" -e "s|=/usr/|=$PREFIX/root/usr/|" "$pc" > "$PREFIX/pc/$(basename "$pc")"
done
echo "unpacked: ${pkgs[*]} -> $PREFIX (pkg-config: $PREFIX/pc)"
ls "$PREFIX/pc"
