#!/usr/bin/env bash
# mk9-lauta (2026-11-13): compiles every source of the game's own (app/src: not the generated game code) for the Linux
# build, a few at a time, and lists the ones that fail with their first errors: the port's to-do list.
set -uo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$REPO/out/linux}
. "$REPO/tools/linux/clang20_env.sh"
mapfile -t objs < <(ninja -C "$OUT" -t targets all 2>/dev/null | grep -oE "^CMakeFiles/(mk9|mk9_renut_gpu)\.dir/[^:]*src/[^:]*\.o" | sort -u)
echo "app objects: ${#objs[@]}"
log="$OUT/compile_app.log"
ninja -C "$OUT" -j "${JOBS:-2}" -k 0 "${objs[@]}" > "$log" 2>&1
echo "== ninja exit $? (log: $log)"
grep -E "^FAILED: " "$log" | sed 's/FAILED: //' | sort -u > "$OUT/compile_app_failed.txt"
echo "failed: $(wc -l < "$OUT/compile_app_failed.txt")"
cat "$OUT/compile_app_failed.txt"
