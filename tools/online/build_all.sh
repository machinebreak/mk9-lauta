#!/usr/bin/env bash
# The whole mk9-nx PC build on Windows, from an extracted and verified disc to out/pc/mk9.exe, step by step:
#   host   the code generator (rexglue) in out/host
#   gen    default.xex -> app/generated (optimised flavour) + post-codegen steps
#   glsl   glslangValidator in out/glslang (the native renderer's own shaders)
#   pc     configure (preset win-pc) and build out/pc
# Every step writes its tools' logs to out\ (host_*.log, codegen.log, glslang_*.log, pc_*.log), prints the free disk
# and memory, and stops the script on the first failure. Steps already done are skipped by the tools themselves
# (ninja), so re-running is cheap. Needs: VS 2022 Build Tools (MSVC + Windows SDK), LLVM clang 20 in ..\_tools\llvm,
# Python 3 with xxhash and cryptography, and the DXC release in
# app\thirdparty\reblue-XenosRecomp\thirdparty\dxc-bin\{bin,lib}\x64 (docs/online/README.md).
#
#   bash tools/online/build_all.sh [host] [gen] [glsl] [pc]      (default: all four, in order)
#   JOBS_SDK=8 JOBS_GAME=4 bash tools/online/build_all.sh pc     (parallel compile jobs; the game's files are large)
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
JOBS_SDK=${JOBS_SDK:-8}
JOBS_GAME=${JOBS_GAME:-4}
# Renut's shader translator (app/thirdparty/reblue-XenosRecomp): worker threads. Each keeps its own DXC (~550 MB with
# MK9's shaders); every core at once held ~7 GB on this 12-thread PC.
export XENOS_RECOMP_JOBS=${XENOS_RECOMP_JOBS:-6}
cd "$REPO"

ps() { powershell.exe -NoProfile -ExecutionPolicy Bypass "$@"; }
status() {
  local mem
  mem=$(powershell.exe -NoProfile -Command '$o = Get-CimInstance Win32_OperatingSystem; "{0:n1} GB RAM free, commit {1:n1}/{2:n1} GB" -f ($o.FreePhysicalMemory/1MB), (($o.TotalVirtualMemorySize-$o.FreeVirtualMemory)/1MB), ($o.TotalVirtualMemorySize/1MB)' | tr -d '\r')
  echo "== $(date '+%H:%M:%S') $* | C: $(df -h /c | tail -1 | awk '{print $4}') free | $mem"
}
run_ps() {  # run_ps <log> <command line>: tools\win\run.ps1 loads the MSVC + LLVM environment
  local log=$1; shift
  ps -File 'tools\win\run.ps1' "$log" "$*" | tail -25
  local code=${PIPESTATUS[0]}
  return "$code"
}

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(host gen glsl pc)
for step in "${steps[@]}"; do
  case $step in
    host)
      status "host: configure the code generator"
      run_ps 'out\host_configure.log' "cmake -S sdk -B out/host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS=-march=x86-64-v2 -DCMAKE_CXX_FLAGS=-march=x86-64-v2"
      status "host: build rexglue (-j $JOBS_SDK)"
      run_ps 'out\host_build.log' "cmake --build out/host --target rexglue -j $JOBS_SDK"
      ;;
    gen)
      status "gen: default.xex -> C++ (optimised flavour)"
      ps -File 'tools\codegen.ps1' -Flavor optimised 2>&1 | tail -20
      [ -f app/generated/rexglue.cmake ] || { echo "codegen produced no app/generated/rexglue.cmake" >&2; exit 1; }
      echo "generated: $(ls app/generated/default/*.cpp | wc -l) files, $(du -sh app/generated | cut -f1)"
      ;;
    glsl)
      status "glsl: glslangValidator"
      ps -File 'tools\win\build_glslang.ps1' | tail -6
      [ -f out/glslang/StandAlone/glslangValidator.exe ] || { echo "glslangValidator.exe missing" >&2; exit 1; }
      ;;
    pc)
      status "pc: configure (preset win-pc)"
      run_ps 'out\pc_configure.log' "cmake --preset win-pc -S app"
      status "pc: build (-j $JOBS_GAME)"
      run_ps 'out\pc_build.log' "cmake --build out/pc -j $JOBS_GAME"
      ls -l out/pc/mk9.exe
      ;;
    *) echo "unknown step $step" >&2; exit 1 ;;
  esac
  status "$step: done"
done
