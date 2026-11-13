/* mk9-lauta (2026-11-13): SHA-256 on Linux (Windows' is BCrypt's): the installer's check of each game file as it is
 * written (~7 GB) and the updater's of its download. The system's libcrypto (OpenSSL 3 or 1.1, opened at run time:
 * the processor's SHA instructions, ~1.7 GB/s) where it is, else libjuice's picohash (~250 MB/s). */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mk9_sha256 mk9_sha256;

/* A hash to add to; NULL only when out of memory. */
mk9_sha256* mk9_sha256_begin(void);
void mk9_sha256_add(mk9_sha256* hash, const void* data, size_t size);
/* The digest; the hash is gone after it. */
void mk9_sha256_end(mk9_sha256* hash, unsigned char digest[32]);

/* A whole file's; 0 when it cannot be read. */
int mk9_sha256_file(const char* path, unsigned char digest[32]);

/* "libcrypto" or "picohash": what hashes (for the log). */
const char* mk9_sha256_engine(void);

#ifdef __cplusplus
}
#endif
