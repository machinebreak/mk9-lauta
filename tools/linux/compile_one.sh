#!/usr/bin/env bash
# mk9-lauta (2026-11-13): compiles single sources of the Linux build (their objects only: little memory), to check a
# port without the whole build. Paths relative to app/: tools/linux/compile_one.sh src/x.cpp ...
# The first run also builds what those objects wait for (the SDK's libraries: ninja's order-only dependencies).
set -uo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$REPO/out/linux}
. "$REPO/tools/linux/clang20_env.sh"
targets=()
for src in "$@"; do targets+=("CMakeFiles/mk9.dir/$src.o"); done
log="$OUT/compile_one.log"
ninja -C "$OUT" -j "${JOBS:-1}" -k 0 "${targets[@]}" > "$log" 2>&1
status=$?
grep -E "error:|FAILED:" "$log" | head -"${LINES_MAX:-60}"
for src in "$@"; do
  if [ -f "$OUT/CMakeFiles/mk9.dir/$src.o" ]; then echo "ok      $src"; else echo "failed  $src"; fi
done
echo "== ninja exit $status (whole log: $log)"
