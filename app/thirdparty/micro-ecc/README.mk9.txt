micro-ecc (github.com/kmackay/micro-ecc, master, fetched 2026-11-13; BSD 2-Clause: LICENSE.txt).
mk9-lauta's Linux build verifies the release signature with it (P-256 ECDSA, src/mk9_updater.cpp); the Windows
build uses BCrypt. Only the portable files are kept (the ARM and AVR assembly is not used on x86-64).
