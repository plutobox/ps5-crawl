/* Downloads over HTTPS, for the music the startup menu fetches
   (startup.cc): the system's own sceHttp and sceSsl, with the system's
   certificate store, checking the file against its SHA-256 (libsodium)
   before it takes the file's name.

   The calls, their order and the option values are SwanStationPS5's update
   check (examples/update-check/update_check.c, UPDATE_CHECK_USE_SCEHTTP),
   run on a console from a sandboxed title like this one. Its
   docs/boilerplate/CURL.md says why not libcurl: in a sandboxed title
   sceHttp needs nothing added, and libcurl needs a resolver, an fcntl
   wrapper and non-blocking sockets of its own. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sodium/crypto_hash_sha256.h>

#include "download.h"

int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceSslInit(size_t pool_size);
int sceSslTerm(int context);
int sceHttpInit(int net_pool, int ssl_context, size_t pool_size);
int sceHttpTerm(int context);
int sceHttpCreateTemplate(int context, const char *user_agent, int version,
                          int auto_proxy);
int sceHttpDeleteTemplate(int template_id);
int sceHttpCreateConnectionWithURL(int template_id, const char *url,
                                   int keep_alive);
int sceHttpDeleteConnection(int connection);
int sceHttpCreateRequestWithURL(int connection, int method, const char *url,
                                uint64_t content_length);
int sceHttpDeleteRequest(int request);
int sceHttpSetResolveTimeOut(int id, uint32_t usec);
int sceHttpSetConnectTimeOut(int id, uint32_t usec);
int sceHttpSetSendTimeOut(int id, uint32_t usec);
int sceHttpSetRecvTimeOut(int id, uint32_t usec);
int sceHttpsEnableOption(int id, uint32_t flags);
int sceHttpSendRequest(int request, const void *data, size_t size);
int sceHttpGetStatusCode(int request, int *status);
int sceHttpGetResponseContentLength(int request, int *result,
                                    uint64_t *length);
int sceHttpReadData(int request, void *data, size_t size);

enum
{
    NET_POOL = 1024 * 1024,
    SSL_POOL = 304 * 1024,
    HTTP_POOL = 4 * 1024 * 1024,
    HTTP_1_1 = 2,
    METHOD_GET = 0,
    TIMEOUT_USEC = 30 * 1000000,
    /* server verify | name check | not-after | not-before | known CA | SNI */
    VERIFY_FLAGS = 0x01 | 0x04 | 0x08 | 0x10 | 0x20 | 0x80,
};

static void hex(const unsigned char *digest, unsigned len, char *out)
{
    for (unsigned i = 0; i < len; i++)
        sprintf(out + 2 * i, "%02x", digest[i]);
}

/* The SHA-256 of a file as 64 hex digits; 0 if it was read. */
int crawl_ps5_file_sha256(const char *path, char out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    crypto_hash_sha256_state sha;
    crypto_hash_sha256_init(&sha);
    unsigned char block[1 << 16];
    size_t n;
    while ((n = fread(block, 1, sizeof block, f)) > 0)
        crypto_hash_sha256_update(&sha, block, n);
    const int failed = ferror(f);
    fclose(f);
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&sha, digest);
    if (failed)
        return -1;
    hex(digest, sizeof digest, out);
    return 0;
}

/* Fetches url into out, hashing it; 0, or a negative sce error, or the HTTP
   status, or 1 if progress cancelled it. */
static int fetch(const char *url, FILE *out, crypto_hash_sha256_state *sha,
                 download_progress progress, void *ctx)
{
    int pool = -1, ssl = -1, http = -1, tmpl = -1, connection = -1;
    int request = -1, status = 0, result;

    result = pool = sceNetPoolCreate("crawl-download", NET_POOL, 0);
    if (result >= 0)
        result = ssl = sceSslInit(SSL_POOL);
    if (result >= 0)
        result = http = sceHttpInit(pool, ssl, HTTP_POOL);
    if (result >= 0)
        result = tmpl = sceHttpCreateTemplate(http, "crawl-ps5", HTTP_1_1, 0);
    if (result >= 0)
        result = sceHttpSetResolveTimeOut(tmpl, TIMEOUT_USEC);
    if (result >= 0)
        result = sceHttpSetConnectTimeOut(tmpl, TIMEOUT_USEC);
    if (result >= 0)
        result = sceHttpSetSendTimeOut(tmpl, TIMEOUT_USEC);
    if (result >= 0)
        result = sceHttpSetRecvTimeOut(tmpl, TIMEOUT_USEC);
    if (result >= 0)
        result = sceHttpsEnableOption(tmpl, VERIFY_FLAGS);
    if (result >= 0)
        result = connection = sceHttpCreateConnectionWithURL(tmpl, url, 0);
    if (result >= 0)
    {
        result = request = sceHttpCreateRequestWithURL(connection, METHOD_GET,
                                                      url, 0);
    }
    if (result >= 0)
        result = sceHttpSendRequest(request, NULL, 0);
    if (result >= 0)
        result = sceHttpGetStatusCode(request, &status);
    if (result >= 0 && status != 200)
        result = status;

    uint64_t total = 0;
    int known = 0;
    if (result == 0
        && sceHttpGetResponseContentLength(request, &known, &total) < 0)
    {
        total = 0;
    }
    int64_t got = 0;
    static unsigned char block[1 << 16];
    while (result == 0)
    {
        const int n = sceHttpReadData(request, block, sizeof block);
        if (n < 0)
            result = n;
        else if (n == 0)
            break;
        else if (fwrite(block, 1, n, out) != (size_t) n)
            result = -EIO;
        else
        {
            crypto_hash_sha256_update(sha, block, n);
            got += n;
            if (progress && progress(ctx, got, (int64_t) total))
                result = 1;
        }
    }

    if (request >= 0)
        sceHttpDeleteRequest(request);
    if (connection >= 0)
        sceHttpDeleteConnection(connection);
    if (tmpl >= 0)
        sceHttpDeleteTemplate(tmpl);
    if (http >= 0)
        sceHttpTerm(http);
    if (ssl >= 0)
        sceSslTerm(ssl);
    if (pool >= 0)
        sceNetPoolDestroy(pool);
    return result;
}

/* Downloads url to path, through path.part, if its SHA-256 is sha256_hex.
   progress (may be NULL) is called as it goes, and cancels it by returning
   nonzero. 0 on success; otherwise err says why. */
int crawl_ps5_download(const char *url, const char *path,
                       const char *sha256_hex, download_progress progress,
                       void *ctx, char *err, size_t errlen)
{
    char part[1024];
    snprintf(part, sizeof part, "%s.part", path);
    FILE *out = fopen(part, "wb");
    if (!out)
    {
        snprintf(err, errlen, "can't write %s: %s", part, strerror(errno));
        return -1;
    }
    crypto_hash_sha256_state sha;
    crypto_hash_sha256_init(&sha);
    const int result = fetch(url, out, &sha, progress, ctx);
    const int closed = fclose(out);
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&sha, digest);
    char got[65];
    hex(digest, sizeof digest, got);

    if (result != 0)
    {
        if (result == 1)
            snprintf(err, errlen, "stopped");
        else if (result > 1)
            snprintf(err, errlen, "the server answered HTTP %d", result);
        else
            snprintf(err, errlen, "error 0x%08x", (unsigned) result);
        printf("Download: %s: %s\n", url, err);
        remove(part);
        return -1;
    }
    if (closed != 0)
    {
        snprintf(err, errlen, "can't write %s", part);
        remove(part);
        return -1;
    }
    if (strcmp(got, sha256_hex) != 0)
    {
        snprintf(err, errlen, "%s has SHA-256 %s, expected %s", url, got,
                 sha256_hex);
        remove(part);
        return -1;
    }
    remove(path);
    if (rename(part, path) != 0)
    {
        snprintf(err, errlen, "can't rename %s: %s", part, strerror(errno));
        remove(part);
        return -1;
    }
    return 0;
}
