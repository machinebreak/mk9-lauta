#!/usr/bin/env bash
# mk9-lauta (2026-11-13): clang 20 for the Linux build, the same major version as the Windows build's (C++23 #embed,
# and the same code generation): the system's (apt install clang-20) if there is one, else the one local_deps.sh
# unpacked into ~/mk9-deps. Source it: . tools/linux/clang20_env.sh   (sets CC, CXX, LD_LIBRARY_PATH)
P=${PREFIX:-$HOME/mk9-deps}/root
if [ -x /usr/lib/llvm-20/bin/clang++ ]; then
  export CC=/usr/lib/llvm-20/bin/clang CXX=/usr/lib/llvm-20/bin/clang++
elif [ -x "$P/usr/lib/llvm-20/bin/clang++" ]; then
  export CC="$P/usr/lib/llvm-20/bin/clang" CXX="$P/usr/lib/llvm-20/bin/clang++"
  export LD_LIBRARY_PATH="$P/usr/lib/llvm-20/lib:$P/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
else
  echo "no clang 20: sudo apt install clang-20 (or tools/linux/local_deps.sh clang-20 libclang-cpp20 libllvm20 libclang-common-20-dev libclang-rt-20-dev llvm-20-linker-tools)" >&2
  return 1 2>/dev/null || exit 1
fi
export PKG_CONFIG_PATH="${PREFIX:-$HOME/mk9-deps}/pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# the -dev packages local_deps.sh unpacked: their .so links for the linker's -l (the SDK passes the names only)
if [ -d "$P/usr/lib/x86_64-linux-gnu" ]; then
  export LIBRARY_PATH="$P/usr/lib/x86_64-linux-gnu${LIBRARY_PATH:+:$LIBRARY_PATH}"
fi
