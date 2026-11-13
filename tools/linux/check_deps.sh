#!/usr/bin/env bash
# mk9-lauta (2026-11-13): the Debian/Ubuntu packages the native Linux build needs, and which are missing here.
pkgs="clang cmake ninja-build pkg-config python3 libx11-dev libx11-xcb-dev libxext-dev libxcursor-dev libxi-dev
libxfixes-dev libxrandr-dev libxss-dev libxtst-dev libxinerama-dev libxkbcommon-dev libwayland-dev libdecor-0-dev
libudev-dev libdbus-1-dev libasound2-dev libpulse-dev libpipewire-0.3-dev libcurl4-openssl-dev xvfb"
missing=""
for p in $pkgs; do
  if dpkg -s "$p" >/dev/null 2>&1; then echo "ok      $p"; else echo "missing $p"; missing="$missing $p"; fi
done
[ -z "$missing" ] && echo "all there" || echo "sudo apt install -y$missing"
