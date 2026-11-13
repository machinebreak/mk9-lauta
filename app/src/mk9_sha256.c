/* mk9-lauta (2026-11-13): SHA-256 on Linux (mk9_sha256.h). picohash is C (not C++) and its functions are static, so
 * nothing here clashes with the SDK's own sha256::SHA256 (inside its kernel's xboxkrnl_crypt.cpp). */
#include "mk9_sha256.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "../thirdparty/libjuice/src/picohash.h"

/* OpenSSL's EVP digest calls, the same in 3.x and 1.1 (EVP_MD_CTX_new and _free exist since 1.1.0) */
typedef struct mk9_evp_ctx mk9_evp_ctx;
typedef struct mk9_evp_md mk9_evp_md;
static struct {
  mk9_evp_ctx* (*ctx_new)(void);
  void (*ctx_free)(mk9_evp_ctx*);
  const mk9_evp_md* (*sha256)(void);
  int (*init)(mk9_evp_ctx*, const mk9_evp_md*, void* engine);
  int (*update)(mk9_evp_ctx*, const void*, size_t);
  int (*final)(mk9_evp_ctx*, unsigned char*, unsigned int*);
  int ok;
} g_ssl;
static pthread_once_t g_ssl_once = PTHREAD_ONCE_INIT;

static void LoadLibcrypto(void) {
  static const char* const kNames[] = {"libcrypto.so.3", "libcrypto.so.1.1", "libcrypto.so"};
  for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
    void* lib = dlopen(kNames[i], RTLD_NOW | RTLD_LOCAL);
    if (!lib) continue;
    *(void**)&g_ssl.ctx_new = dlsym(lib, "EVP_MD_CTX_new");
    *(void**)&g_ssl.ctx_free = dlsym(lib, "EVP_MD_CTX_free");
    *(void**)&g_ssl.sha256 = dlsym(lib, "EVP_sha256");
    *(void**)&g_ssl.init = dlsym(lib, "EVP_DigestInit_ex");
    *(void**)&g_ssl.update = dlsym(lib, "EVP_DigestUpdate");
    *(void**)&g_ssl.final = dlsym(lib, "EVP_DigestFinal_ex");
    if (g_ssl.ctx_new && g_ssl.ctx_free && g_ssl.sha256 && g_ssl.init && g_ssl.update && g_ssl.final) {
      g_ssl.ok = 1;
      return;  /* kept open for the process's life */
    }
    dlclose(lib);
  }
}

struct mk9_sha256 {
  mk9_evp_ctx* evp; /* NULL: picohash */
  picohash_ctx_t pico;
};

mk9_sha256* mk9_sha256_begin(void) {
  pthread_once(&g_ssl_once, LoadLibcrypto);
  mk9_sha256* hash = (mk9_sha256*)calloc(1, sizeof(mk9_sha256));
  if (!hash) return NULL;
  if (g_ssl.ok) {
    hash->evp = g_ssl.ctx_new();
    if (hash->evp && g_ssl.init(hash->evp, g_ssl.sha256(), NULL) != 1) {
      g_ssl.ctx_free(hash->evp);
      hash->evp = NULL;
    }
  }
  if (!hash->evp) picohash_init_sha256(&hash->pico);
  return hash;
}

void mk9_sha256_add(mk9_sha256* hash, const void* data, size_t size) {
  if (!hash || !size) return;
  if (hash->evp) {
    g_ssl.update(hash->evp, data, size);
  } else {
    picohash_update(&hash->pico, data, size);
  }
}

void mk9_sha256_end(mk9_sha256* hash, unsigned char digest[32]) {
  if (!hash) return;
  if (hash->evp) {
    unsigned int length = 32;
    g_ssl.final(hash->evp, digest, &length);
    g_ssl.ctx_free(hash->evp);
  } else {
    picohash_final(&hash->pico, digest);
  }
  free(hash);
}

int mk9_sha256_file(const char* path, unsigned char digest[32]) {
  FILE* file = fopen(path, "rb");
  if (!file) return 0;
  const size_t size = 1u << 20;
  unsigned char* buffer = (unsigned char*)malloc(size);
  mk9_sha256* hash = mk9_sha256_begin();
  if (!buffer || !hash) {
    free(buffer);
    if (hash) mk9_sha256_end(hash, digest);
    fclose(file);
    return 0;
  }
  size_t n;
  while ((n = fread(buffer, 1, size, file)) > 0) mk9_sha256_add(hash, buffer, n);
  const int ok = !ferror(file);
  free(buffer);
  fclose(file);
  mk9_sha256_end(hash, digest);
  return ok;
}

const char* mk9_sha256_engine(void) {
  pthread_once(&g_ssl_once, LoadLibcrypto);
  return g_ssl.ok ? "libcrypto" : "picohash";
}
