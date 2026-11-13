#!/usr/bin/env bash
# mk9-lauta (2026-11-13): the native Linux build, step by step (from WSL2 on the Windows machine, or any Linux box):
#   glsl   glslangValidator for the host (out/glslang-linux), the build's SPIR-V initializers
#   conf   configure out/linux (Ninja, clang, RelWithDebInfo, x86-64-v2: the same flags as the Windows build)
#   build  build out/linux (JOBS: the game's generated files are large: 4 at a time by default)
# The game code is the Windows build's app/generated (platform independent C++), its shaders the Windows build's SPIR-V
# (tools/renut_linux_shader_cache.py). Steps already done are skipped by the tools themselves (ninja), so running it
# again is cheap. Packages: tools/linux/check_deps.sh (or tools/linux/local_deps.sh without root).
#
#   tools/linux/build_linux.sh [glsl] [conf] [build]      (default: all three)
#   JOBS=6 tools/linux/build_linux.sh build
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OUT=${OUT:-$REPO/out/linux}
GLSL=${GLSL:-$REPO/out/glslang-linux}
JOBS=${JOBS:-4}
# clang 20, the Windows build's major version (C++23 #embed, the same code generation): tools/linux/clang20_env.sh
. "$REPO/tools/linux/clang20_env.sh"
status() { echo "== $(date '+%H:%M:%S') $* | free: $(df -h "$REPO" | tail -1 | awk '{print $4}') | mem: $(free -g | awk '/Mem/ {print $7}') GB"; }

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(glsl conf build)
for step in "${steps[@]}"; do
  case $step in
    glsl)
      status "glsl: glslangValidator"
      cmake -S "$REPO/sdk/thirdparty/glslang" -B "$GLSL" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF \
        -DENABLE_GLSLANG_BINARIES=ON >/dev/null
      cmake --build "$GLSL" --target glslangValidator -j "$JOBS" | tail -2
      ls -la "$GLSL/StandAlone/glslang" "$GLSL/StandAlone/glslangValidator" 2>/dev/null || true
      ;;
    conf)
      status "conf: out/linux"
      validator="$GLSL/StandAlone/glslangValidator"
      [ -x "$validator" ] || validator="$GLSL/StandAlone/glslang"
      cmake -S "$REPO/app" -B "$OUT" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2 \
        -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_VERSION_OVERRIDE=1.0.4 \
        -DSA2_GLSLANG_VALIDATOR="$validator" ${CONF_EXTRA:-} 2>&1 | tail -25
      ;;
    build)
      status "build: out/linux (-j $JOBS)"
      # -k 0: every error of the pass at once (KEEP=1 stops at the first)
      cmake --build "$OUT" -j "$JOBS" -- -k "${KEEP:-0}" 2>&1 | tail -${TAIL:-40}
      ;;
    *) echo "unknown step $step" >&2; exit 2 ;;
  esac
done
status "done"
