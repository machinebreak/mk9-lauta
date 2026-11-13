miniz 3.1.2 (github.com/richgel999/miniz, release asset miniz-3.1.2.zip, sha256
f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a; fetched 2026-11-13; MIT: LICENSE).
mk9-lauta's Linux build unpacks the Kratos port's .zip with it (src/mk9_updater.cpp, UnpackZip): Windows' build uses
tar.exe or PowerShell. Built reading only, without zlib's names, hidden in the executable (app/CMakeLists.txt).
