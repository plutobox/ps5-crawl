/* HTTPS downloads for the PS5 title (download.c): the system's sceHttp. */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Where the music the startup menu downloads goes, as <folder>/music/<file>
   beside the sound pack's own (initfile.cc reads it from both). */
#define CRAWL_PS5_DOWNLOAD_DIR "/data/crawl/"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*download_progress)(void *ctx, int64_t now, int64_t total);

int crawl_ps5_file_sha256(const char *path, char out[65]);
int crawl_ps5_download(const char *url, const char *path,
                       const char *sha256_hex, download_progress progress,
                       void *ctx, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
